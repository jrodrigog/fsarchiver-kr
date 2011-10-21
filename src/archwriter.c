/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2010 Francois Dupoux.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Homepage: http://www.fsarchiver.org
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/mtio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/major.h>
#include <assert.h>

#include "fsarchiver.h"
#include "dico.h"
#include "common.h"
#include "options.h"
#include "archwriter.h"
#include "queue.h"
#include "writebuf.h"
#include "comp_gzip.h"
#include "comp_bzip2.h"
#include "error.h"

int archwriter_precache(carchwriter *ai, u64 size);
int archwriter_write_regular(carchwriter *ai, void *data, s64 size);
int archwriter_write_blocks(carchwriter *ai, void *data, s64 size);
int archwriter_write_flush(carchwriter *ai, void *data, s64 size);
int archwriter_check_disk_space(carchwriter *ai);

int archwriter_init(carchwriter *ai)
{
    assert(ai);
    memset(ai, 0, sizeof(struct s_archwriter));
    strlist_init(&ai->vollist);
    ai->newarch=false;
    ai->archfd=-1;
    ai->archid=0;
    ai->curvol=0;
    ai->currentpos=0;
    ai->devblocksize=0;
    ai->cache=NULL;
    ai->cachewrite=NULL;
    ai->cachesize=0;
    ai->write=NULL;
    ai->originaltapeblocksize=-1;
    return 0;
}

int archwriter_destroy(carchwriter *ai)
{
    assert(ai);
    strlist_destroy(&ai->vollist);
    return 0;
}

int archwriter_generate_id(carchwriter *ai)
{
    assert(ai);
    ai->archid=generate_random_u32_id();
    return 0;
}

int archwriter_create(carchwriter *ai)
{
    struct stat64 st;
    struct mtget status;
    struct mtop operation;
    struct sockaddr_un address;
    long archflags=0;
    long archperm;
    int res;
    
    assert(ai);
    
    // init
    memset(&st, 0, sizeof(st));
    archflags=O_RDWR|O_LARGEFILE;
    archperm=S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH;
    
    if (strcmp(ai->volpath, "-")==0)
    {   // go fully unbuffered on piped operation
        setvbuf(stdout, 0, _IONBF, 0);
        ai->archfd=STDOUT_FILENO;
        ai->write=&archwriter_write_flush;
    }
    else
    {
        // get stats only for regular files
        res=stat64(ai->volpath, &st);
        if ((g_options.overwrite==0) && (res==0) && S_ISREG(st.st_mode))
        {   errprintf("%s already exists, please remove it first.\n", ai->basepath);
            return -1;
        }
        else if ((res!=0) || S_ISREG(st.st_mode))
        {   // there is no file, create it
            ai->write=&archwriter_write_regular;
            archflags|=O_CREAT|O_TRUNC;
            ai->newarch=true;
            path_force_extension(ai->basepath, PATH_MAX, ai->basepath, ".fsa");
        }
        else if (S_ISBLK(st.st_mode))
        {
            ai->write=&archwriter_write_blocks;
            ai->devblocksize=st.st_blksize;
        }
        else if (S_ISSOCK(st.st_mode))
        {
            ai->write=&archwriter_write_flush;
            
            if ((ai->archfd=socket(PF_UNIX, SOCK_STREAM, 0))<0)
            {   errprintf("cannot create a socket\n");
                return 1;
            }

            // setup the address structure
            memset(&address, 0, sizeof(struct sockaddr_un));
            address.sun_family = AF_UNIX;
            snprintf(address.sun_path, UNIX_PATH_MAX, "%s", ai->volpath);

            if (connect(ai->archfd, (struct sockaddr *) &address, sizeof(struct sockaddr_un))!=0)
            {   printf("cannot connect to %s\n", ai->volpath);
                return 1;
            }
        }
        else if (S_ISCHR(st.st_mode) || S_ISFIFO(st.st_mode))
        {
            ai->write=&archwriter_write_flush;
        }
        else
        {   errprintf("%s is not a file that can be handled\n", ai->basepath);
            return -1;
        }
     
        if (ai->archfd==-1)
        {
            ai->archfd=open64(ai->volpath, archflags, archperm);
            if (ai->archfd < 0)
            {   sysprintf ("cannot create archive %s\n", ai->volpath);
                return -1;
            }
        }
        
        if (S_ISCHR(st.st_mode) && (major(st.st_rdev)==SCSI_TAPE_MAJOR))
        {
            if (ioctl(ai->archfd, MTIOCGET, (char *)&status)<0) 
            {   errprintf("cannot get the tape status for %s ioctl() failed\n", ai->basepath);
                close(ai->archfd);
                return -1;
            }
            ai->write=&archwriter_write_blocks;
            ai->devblocksize=FSA_TAPE_BLOCK;
            ai->originaltapeblocksize=(status.mt_gstat>>MT_ST_BLKSIZE_SHIFT)&MT_ST_BLKSIZE_MASK;
            if (ai->devblocksize!=ai->originaltapeblocksize)
            {
                operation.mt_op=MTSETBLK;
                operation.mt_count=ai->devblocksize&MT_ST_BLKSIZE_MASK;
                if (ioctl(ai->archfd, MTIOCTOP, (char *)&operation)<0) 
                {   errprintf("cannot set the tape block size to %d ioctl() failed\n", operation.mt_count);
                    close(ai->archfd);
                    return -1;
                }            
            }
        }
    }
        
    strlist_add(&ai->vollist, ai->volpath);
    ai->currentpos=0;
    msgprintf(MSG_DEBUG2,"block size at: %ld\n", ai->devblocksize);

    return 0;
}

int archwriter_close(carchwriter *ai)
{
    struct mtop operation;
    long lres, pending;

    assert(ai);
    
    if (ai->archfd<0)
        return -1;

    pending=ai->cachewrite-ai->cache;
    if (ai->cache && pending)
    {
	memset(ai->cachewrite, 0, ai->devblocksize-pending);
        if ((lres=write(ai->archfd, ai->cache, ai->devblocksize))!=ai->devblocksize)
        {   
            errprintf("flush bytes: write(%ld)=%ld failed\n", ai->devblocksize, lres);
            return -1;
        }
    }

    fsync(ai->archfd); // just in case the user reboots after it exits

    // restore the original tape block size, if required
    if ((ai->originaltapeblocksize!=-1) && (ai->originaltapeblocksize!=ai->devblocksize))
    {
        operation.mt_op=MTSETBLK;
        operation.mt_count=ai->originaltapeblocksize&MT_ST_BLKSIZE_MASK;
        if ((lres=ioctl(ai->archfd, MTIOCTOP, (char *)&operation))<0) 
        {
            errprintf("cannot set the tape block size to %d: ioctl()=%ld failed\n", operation.mt_count, lres);
        }
    }
    
    close(ai->archfd);
    ai->archfd=-1;
   
    // free the cache buffer if it has been allocated
    if (ai->cache)
    {
        free(ai->cache);
        ai->cache=NULL;
        ai->cachewrite=NULL;
        ai->cachesize=0;
    }
    
    return 0;
}

int archwriter_remove(carchwriter *ai)
{
    char volpath[PATH_MAX];
    int count;
    int i;
    
    assert(ai);
    
    if (ai->archfd >= 0)
    {
        archwriter_close(ai);
    }
    
    if (ai->newarch==true)
    {
        count=strlist_count(&ai->vollist);
        for (i=0; i < count; i++)
        {
            if (strlist_getitem(&ai->vollist, i, volpath, sizeof(volpath))==0)
            {
                if (unlink(volpath)==0)
                    msgprintf(MSG_FORCE, "removed %s\n", volpath);
                else
                    errprintf("cannot remove %s\n", volpath);
            }
        }
    }
    return 0;
}

s64 archwriter_get_currentpos(carchwriter *ai)
{
    assert(ai);
    return ai->currentpos;
}

int archwriter_write_buffer(carchwriter *ai, struct s_writebuf *wb)
{
    assert(ai);
    
    if (wb->size<0)
    {   // something is not going well if we have to write negative bytes
        return -1;
    }
    else if (wb->size==0)
    {   // no data to be written
        return 0;
    }
    
    return ai->write(ai, wb->data, wb->size);
}

int archwriter_volpath(carchwriter *ai)
{
    assert(ai);
    return get_path_to_volume(ai->volpath, PATH_MAX, ai->basepath, ai->curvol);
}

int archwriter_is_path_to_curvol(carchwriter *ai, char *path)
{
    assert(ai);
    assert(path);
    return strncmp(ai->volpath, path, PATH_MAX)==0 ? true : false;
}

int archwriter_incvolume(carchwriter *ai, bool waitkeypress)
{
    assert(ai);
    ai->curvol++;
    return archwriter_volpath(ai);
}

int archwriter_write_volheader(carchwriter *ai)
{
    struct s_writebuf *wb=NULL;
    cdico *voldico;
    
    assert(ai);
    
    if ((wb=writebuf_alloc())==NULL)
    {   msgprintf(MSG_STACK, "writebuf_alloc() failed\n");
        return -1;
    }
    
    if ((voldico=dico_alloc())==NULL)
    {   msgprintf(MSG_STACK, "voldico=dico_alloc() failed\n");
        return -1;
    }
    
    // prepare header
    dico_add_u32(voldico, 0, VOLUMEHEADKEY_VOLNUM, ai->curvol);
    dico_add_u32(voldico, 0, VOLUMEHEADKEY_ARCHID, ai->archid);
    dico_add_string(voldico, 0, VOLUMEHEADKEY_FILEFORMATVER, FSA_FILEFORMAT);
    dico_add_string(voldico, 0, VOLUMEHEADKEY_PROGVERCREAT, FSA_VERSION);
    
    // write header to buffer
    if (writebuf_add_header(wb, voldico, FSA_MAGIC_VOLH, ai->archid, FSA_FILESYSID_NULL)!=0)
    {   errprintf("archio_write_header() failed\n");
        return -1;
    }
    
    // write header to file
    if (archwriter_write_buffer(ai, wb)!=0)
    {   errprintf("archwriter_write_buffer() failed\n");
        return -1;
    }
    
    dico_destroy(voldico);
    writebuf_destroy(wb);
    
    return 0;
}

int archwriter_write_volfooter(carchwriter *ai, bool lastvol)
{
    struct s_writebuf *wb=NULL;
    cdico *voldico;
    
    assert(ai);
    
    if ((wb=writebuf_alloc())==NULL)
    {   errprintf("writebuf_alloc() failed\n");
        return -1;
    }
    
    if ((voldico=dico_alloc())==NULL)
    {   errprintf("voldico=dico_alloc() failed\n");
        return -1;
    }
    
    // prepare header
    dico_add_u32(voldico, 0, VOLUMEFOOTKEY_VOLNUM, ai->curvol);
    dico_add_u32(voldico, 0, VOLUMEFOOTKEY_ARCHID, ai->archid);
    dico_add_u32(voldico, 0, VOLUMEFOOTKEY_LASTVOL, lastvol);
    
    // write header to buffer
    if (writebuf_add_header(wb, voldico, FSA_MAGIC_VOLF, ai->archid, FSA_FILESYSID_NULL)!=0)
    {   msgprintf(MSG_STACK, "archio_write_header() failed\n");
        return -1;
    }
    
    // write header to file
    if (archwriter_write_buffer(ai, wb)!=0)
    {   msgprintf(MSG_STACK, "archwriter_write_buffer(size=%ld) failed\n", (long)wb->size);
        return -1;
    }
    
    dico_destroy(voldico);
    writebuf_destroy(wb);
    
    return 0;
}

int archwriter_split_check(carchwriter *ai, struct s_writebuf *wb)
{
    s64 cursize;
    
    assert(ai);

    if (((cursize=archwriter_get_currentpos(ai))>=0) && (g_options.splitsize>0 && cursize+wb->size > g_options.splitsize))
    {
        msgprintf(MSG_DEBUG4, "splitchk: YES --> cursize=%lld, g_options.splitsize=%lld, cursize+wb->size=%lld, wb->size=%lld\n",
            (long long)cursize, (long long)g_options.splitsize, (long long)cursize+wb->size, (long long)wb->size);
        return true;
    }
    else
    {
        msgprintf(MSG_DEBUG4, "splitchk: NO --> cursize=%lld, g_options.splitsize=%lld, cursize+wb->size=%lld, wb->size=%lld\n",
            (long long)cursize, (long long)g_options.splitsize, (long long)cursize+wb->size, (long long)wb->size);
        return false;
    }
}

int archwriter_split_if_necessary(carchwriter *ai, struct s_writebuf *wb)
{
    assert(ai);

    if (archwriter_split_check(ai, wb)==true)
    {
        if (archwriter_write_volfooter(ai, false)!=0)
        {   msgprintf(MSG_STACK, "cannot write volume footer: archio_write_volfooter() failed\n");
            return -1;
        }
        archwriter_close(ai);
        archwriter_incvolume(ai, false);
        msgprintf(MSG_VERB2, "Creating new volume: [%s]\n", ai->volpath);
        if (archwriter_create(ai)!=0)
        {   msgprintf(MSG_STACK, "archwriter_create() failed\n");
            return -1;
        }
        if (archwriter_write_volheader(ai)!=0)
        {   msgprintf(MSG_STACK, "cannot write volume header: archio_write_volheader() failed\n");
            return -1;
        }
    }
    return 0;
}

int archwriter_dowrite_block(carchwriter *ai, struct s_blockinfo *blkinfo)
{
    struct s_writebuf *wb=NULL;
    
    assert(ai);

    if ((wb=writebuf_alloc())==NULL)
    {   errprintf("writebuf_alloc() failed\n");
        return -1;
    }
    
    if (writebuf_add_block(wb, blkinfo, ai->archid, blkinfo->blkfsid)!=0)
    {   msgprintf(MSG_STACK, "archio_write_block() failed\n");
        return -1;
    }
    
    if (archwriter_split_if_necessary(ai, wb)!=0)
    {   msgprintf(MSG_STACK, "archwriter_split_if_necessary() failed\n");
        return -1;
    }
    
    if (archwriter_write_buffer(ai, wb)!=0)
    {   msgprintf(MSG_STACK, "archwriter_write_buffer() failed\n");
        return -1;
    }

    writebuf_destroy(wb);
    return 0;
}

int archwriter_dowrite_header(carchwriter *ai, struct s_headinfo *headinfo)
{
    struct s_writebuf *wb=NULL;
    
    assert(ai);

    if ((wb=writebuf_alloc())==NULL)
    {   errprintf("writebuf_alloc() failed\n");
        return -1;
    }
    
    if (writebuf_add_header(wb, headinfo->dico, headinfo->magic, ai->archid, headinfo->fsid)!=0)
    {   msgprintf(MSG_STACK, "archio_write_block() failed\n");
        return -1;
    }
    
    if (archwriter_split_if_necessary(ai, wb)!=0)
    {   msgprintf(MSG_STACK, "archwriter_split_if_necessary() failed\n");
        return -1;
    }
    
    if (archwriter_write_buffer(ai, wb)!=0)
    {   msgprintf(MSG_STACK, "archwriter_write_buffer() failed\n");
        return -1;
    }
    
    writebuf_destroy(wb);
    return 0;
}

int archwriter_check_disk_space(carchwriter *ai)
{
    assert(ai);
    struct statvfs64 statvfsbuf;
    char textbuf[128];
    
    if (fstatvfs64(ai->archfd, &statvfsbuf)!=0)
    {   sysprintf("fstatvfs(fd=%d) failed\n", ai->archfd);
            return -1;
    }

    u64 freebytes = statvfsbuf.f_bfree * statvfsbuf.f_bsize;
    errprintf("Can't write to the archive file. Space on device is %s. \n"
            "If the archive is being written to a FAT filesystem, you may have reached \n"
            "the maximum filesize that it can handle (in general 2 GB)\n", 
            format_size(freebytes, textbuf, sizeof(textbuf), 'h'));
    
    return 0;
}

int archwriter_precache(carchwriter *ai, u64 size)
{
    u8* victim;
    s64 offsetwrite, xmod;
    
    assert(ai);
    
    if ((ai->cachewrite+size)<=(ai->cache+ai->cachesize))
    {   // there is enough room for the new data
        return 0;
    }
    
    // grow the buffer
    victim=ai->cache;
    offsetwrite=ai->cachewrite-ai->cache;
    ai->cachesize=((size/g_options.datablocksize)+1)*g_options.datablocksize;
    xmod=ai->cachesize%ai->devblocksize;
    ai->cachesize+=xmod==0?0:ai->devblocksize-xmod;
    assert((ai->cachesize%ai->devblocksize)==0);
    
    if ((ai->cache=malloc(ai->cachesize))==NULL)
    {   errprintf("cannot allocate block: malloc(%d) failed\n", ai->cachesize);
        return -1;
    }
    
    if (victim)
    {   // copy the previously buffered data
        memcpy(ai->cache, victim, offsetwrite);
        free(victim);
    }

    ai->cachewrite=ai->cache+offsetwrite;
    msgprintf(MSG_DEBUG2,"cache buffer at: %d\n",ai->cachesize);
    
    return 0;
}

int archwriter_write_blocks(carchwriter *ai, void *data, s64 size)
{
    long lres;
    s64 xsize, pending;
    
    assert(ai);
    
    if ((lres=archwriter_precache(ai, size))!=FSAERR_SUCCESS)
    {   errprintf("archwriter_precache(%ld) failed\n", size);
        return -1; 
    }

    // copy the data to the output cache
    memcpy(ai->cachewrite, data, size);
    ai->cachewrite+=size;
    ai->currentpos+=size;
    
    xsize=ai->cachewrite-ai->cache;
    if (xsize>=ai->devblocksize)
    {
        pending=xsize%ai->devblocksize;
        xsize-=pending;
        if ((lres=write(ai->archfd, ai->cache, xsize))!=xsize)
        {   sysprintf("write(size=%ld)=%ld failed\n", xsize, lres);
            return -1;
        }
        memcpy(ai->cache, ai->cache+xsize, pending);
        ai->cachewrite=ai->cache+pending;
    }
    
    return 0;
}

int archwriter_write_regular(carchwriter *ai, void *data, s64 size)
{
    long lres;
    
    assert(ai);
    
    if ((lres=write(ai->archfd, data, size))!=size)
    {
        if ((lres>=0) && (lres<size))
        {
            archwriter_check_disk_space(ai);
            return -1;
        }
        else {
            errprintf("write(size=%ld)=%ld failed\n", size, lres);
            return -1;
        }
    }
    
    ai->currentpos+=size;
    
    return 0;
}

int archwriter_write_flush(carchwriter *ai, void *data, s64 size)
{
    long lres;
    
    assert(ai);
    
    if ((lres=write(ai->archfd, data, size))!=size)
    {   errprintf("write(size=%ld)=%ld failed\n", size, lres);
        return -1;
    }
    fsync(ai->archfd);
    
    ai->currentpos+=size;
    
    return 0;
}
