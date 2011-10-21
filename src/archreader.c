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

#include <errno.h>
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
#include "archreader.h"
#include "queue.h"
#include "comp_gzip.h"
#include "comp_bzip2.h"
#include "error.h"
#include "syncthread.h"

s64 archreader_read_blocks_raw(carchreader *ai, int fd, void *buf, u64 size);
s64 archreader_read_select_raw(carchreader *ai, int fd, void *buf, u64 size);
int archreader_reset_block(carchreader *ai, struct s_blockinfo *out_blkinfo, int *out_sumok); 
int archreader_fmt_version(carchreader *ai, void *volhead);
int archreader_precache(carchreader *ai, s64 size);
int archreader_unread(carchreader *ai, s64 size);
int archreader_skip_regular(carchreader *ai, s64 offset);
int archreader_skip_select(carchreader *ai, s64 offset);
int archreader_skip_blocks(carchreader *ai, s64 offset);
int archreader_skip_st(carchreader *ai, s64 offset);
int archreader_read_select(carchreader *ai, u64 size);
int archreader_read_blocks(carchreader *ai, u64 size);
int archreader_read_regular(carchreader *ai, u64 size);

int archreader_init(carchreader *ai)
{
    assert(ai);
    memset(ai, 0, sizeof(struct s_archreader));
    ai->cryptalgo=ENCRYPT_NULL;
    ai->compalgo=COMPRESS_NULL;
    ai->fsacomp=-1;
    ai->complevel=-1;
    ai->archfd=-1;
    ai->archid=0;
    ai->curvol=0;
    ai->filefmtver=0;
    ai->hasdirsinfohead=false;
    ai->cache=NULL;
    ai->cacheread=NULL;
    ai->cachewrite=NULL;
    ai->devblocksize=0;
    ai->cachesize=0;
    ai->originaltapeblocksize=-1;
    ai->polling=true;
    return 0;
}

int archreader_destroy(carchreader *ai)
{
    assert(ai);
    
    // free the cache buffer if it has been allocated
    if (ai->cache)
    {
        free(ai->cache);
        ai->cache=NULL;
        ai->cacheread=NULL;
        ai->cachewrite=NULL;
        ai->cachesize=0;
    }
    
    return 0;
}

int archreader_open(carchreader *ai)
{   
    int flags;
    struct stat64 st;
    struct mtget status;
    struct mtop operation;
    struct sockaddr_un address;
    int archflags;
    
    assert(ai);
    
    archflags=O_RDONLY|O_LARGEFILE;
    if (strcmp(ai->volpath, "-")==0)
    {   // go fully unbuffered on piped operation
        setvbuf(stdin, 0, _IONBF, 0);
        ai->archfd=STDIN_FILENO;
        ai->read=&archreader_read_select;
        ai->skip=&archreader_skip_select;
        ai->polling=false;
        // check the archive volume is a regular file
        if (fstat64(ai->archfd, &st)!=0)
        {   sysprintf("fstat64(%s) failed\n", ai->volpath);
            return -1;
        }
        ai->devblocksize=st.st_blksize;
    }
    else
    {   
        // check the archive volume is a regular file
        if (stat64(ai->volpath, &st)!=0)
        {   sysprintf("stat64(%s) failed\n", ai->volpath);
            return -1;
        }
        if (S_ISREG(st.st_mode))
        {
            ai->read=&archreader_read_regular;
            ai->skip=&archreader_skip_regular;
            ai->devblocksize=st.st_blksize;
        }
        else if (S_ISBLK(st.st_mode))
        {
            ai->read=&archreader_read_blocks;
            ai->skip=&archreader_skip_blocks;
            ai->devblocksize=st.st_blksize;
        }
        else if (S_ISCHR(st.st_mode))
        {
            archflags|=O_NONBLOCK;
            ai->read=&archreader_read_select;
            ai->skip=&archreader_skip_select;
            ai->devblocksize=st.st_blksize;
        }
        else if (S_ISSOCK(st.st_mode))
        {
            //archflags|=O_NONBLOCK;            
            ai->read=&archreader_read_select;
            ai->skip=&archreader_skip_select;
            ai->devblocksize=st.st_blksize;
            
            if ((ai->archfd=socket(PF_UNIX, SOCK_STREAM, 0))<0)
            {   errprintf("cannot create a socket\n");
                return 1;
            }

            if ((flags = fcntl(ai->archfd, F_GETFL, 0))<0)
            {   errprintf("cannot get socket flags\n");
                return 1;
            }
            
            if (fcntl(ai->archfd, F_SETFL, flags | O_NONBLOCK)<0)
            {   errprintf("cannot set socket flags\n");
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
        else if (S_ISFIFO(st.st_mode))
        {
            archflags|=O_NONBLOCK;
            ai->read=&archreader_read_select;
            ai->skip=&archreader_skip_select;
            ai->devblocksize=st.st_blksize;
        }
        else
        {
            errprintf("%s is not handled file type, cannot continue\n", ai->volpath);
            return -1;
        }

        // open the archive volume
        if (ai->archfd==-1)
        {
            ai->archfd=open64(ai->volpath, archflags);
            if (ai->archfd<0)
            {   sysprintf ("cannot open archive %s\n", ai->volpath);
                return -1;
            }
        }
           
        // test if the device is a scsi tape
        if (S_ISCHR(st.st_mode) && (major(st.st_rdev)==SCSI_TAPE_MAJOR))
        {
            ai->read=&archreader_read_blocks;
            ai->skip=&archreader_skip_st;
            if (ioctl(ai->archfd, MTIOCGET, (char *)&status)<0) 
            {   errprintf("cannot get the tape status for %s ioctl() failed\n", ai->basepath);
                close(ai->archfd);
                return -1;
            }
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
    
    msgprintf(MSG_DEBUG2,"block size at: %ld\n", ai->devblocksize);
    
    return 0;
}

int archreader_close(carchreader *ai)
{
    struct mtop operation;
    
    assert(ai);
    
    if (ai->archfd<0)
        return -1;
    
    // restore the original tape block size, if required
    if ((ai->originaltapeblocksize!=-1) && (ai->devblocksize!=ai->originaltapeblocksize))
    {
        operation.mt_op=MTSETBLK;
        operation.mt_count=ai->originaltapeblocksize&MT_ST_BLKSIZE_MASK;
        if (ioctl(ai->archfd, MTIOCTOP, (char *)&operation)<0) 
        {   errprintf("cannot set the tape block size to %d ioctl() failed\n", operation.mt_count);
            close(ai->archfd);
            return -1;
        }            
    }

    close(ai->archfd);
    ai->archfd=-1;

    return 0;
}

int archreader_volpath(carchreader *ai)
{
    int res;
    res=get_path_to_volume(ai->volpath, PATH_MAX, ai->basepath, ai->curvol);
    return res;
}

int archreader_incvolume(carchreader *ai, bool waitkeypress)
{
    assert(ai);
    ai->curvol++;
    return archreader_volpath(ai);
}

int archreader_read_data(carchreader *ai, void *data, u64 size)
{
    assert(ai);
     
    if (size<0)
    {   // something is not going well if we have to read negative bytes
        return -1;
    }
    else if (size==0)
    {   // no data to be read
        return 0;
    }
    
    if (ai->read(ai, size)!=FSAERR_SUCCESS)
    {   errprintf("cannot read data: read(%ld)\n", size);
        return -1;
    }
    
    memcpy(data, ai->cacheread, size);
    ai->cacheread+=size;
    
    return 0;
}

int archreader_read_dico(carchreader *ai, cdico *d)
{
    u16 size;
    u32 headerlen;
    u32 origsum;
    u32 newsum;
    u8 *buffer;
    u8 *bufpos;
    u16 temp16;
    u32 temp32;
    u8 section;
    u16 count;
    u8 type;
    u16 key;
    int i;
    
    assert(ai);
    assert(d);
    
    // header-len, header-data, header-checksum
    switch (ai->filefmtver)
    {
        case 1:
            if (archreader_read_data(ai, &temp16, sizeof(temp16))!=FSAERR_SUCCESS)
            {   errprintf("imgdisk_read_data() failed\n");
                return OLDERR_FATAL;
            }
            headerlen=le16_to_cpu(temp16);
            break;
        case 2:
            if (archreader_read_data(ai, &temp32, sizeof(temp32))!=FSAERR_SUCCESS)
            {   errprintf("imgdisk_read_data() failed\n");
                return OLDERR_FATAL;
            }
            headerlen=le32_to_cpu(temp32);
            break;
        default:
            errprintf("Fatal error: invalid file format version: ai->filefmtver=%d\n", ai->filefmtver);
            return OLDERR_FATAL;
    }
    
    bufpos=buffer=malloc(headerlen);
    if (!buffer)
    {   errprintf("cannot allocate memory for header\n");
        return FSAERR_ENOMEM;
    }
    
    if (archreader_read_data(ai, buffer, headerlen)!=FSAERR_SUCCESS)
    {   errprintf("cannot read header data\n");
        free(buffer);
        return OLDERR_FATAL;
    }
    
    if (archreader_read_data(ai, &temp32, sizeof(temp32))!=FSAERR_SUCCESS)
    {   errprintf("cannot read header checksum\n");
        free(buffer);
        return OLDERR_FATAL;
    }
    origsum=le32_to_cpu(temp32);
    
    // check header-data integrity using checksum    
    newsum=fletcher32(buffer, headerlen);
    
    if (newsum!=origsum)
    {   errprintf("bad checksum for header\n");
        free(buffer);
        return OLDERR_MINOR; // header corrupt --> skip file
    }
    
    // read count from buffer
    memcpy(&temp16, bufpos, sizeof(temp16));
    bufpos+=sizeof(temp16);
    count=le16_to_cpu(temp16);
    
    // read items
    for (i=0; i < count; i++)
    {
        // a. read type from buffer
        memcpy(&type, bufpos, sizeof(type));
        bufpos+=sizeof(section);
        
        // b. read section from buffer
        memcpy(&section, bufpos, sizeof(section));
        bufpos+=sizeof(section);
        
        // c. read key from buffer
        memcpy(&temp16, bufpos, sizeof(temp16));
        bufpos+=sizeof(temp16);
        key=le16_to_cpu(temp16);
        
        // d. read sizeof(data)
        memcpy(&temp16, bufpos, sizeof(temp16));
        bufpos+=sizeof(temp16);
        size=le16_to_cpu(temp16);
        
        // e. add item to dico
        if (dico_add_generic(d, section, key, bufpos, size, type)!=0)
            return OLDERR_FATAL;
        bufpos+=size;
    }
    
    free(buffer);
    return FSAERR_SUCCESS;
}

int archreader_debug_stream(const char* name, char* stream, s64 size)
{
    char* buf = malloc(size+1);
    buf[size] = '\0';
    for (int i=0; i < size; i++){
        if (stream[i] >= ' ' && stream[i] < '~')
            buf[i]=stream[i];
        else
            buf[i]='.';
    }
    msgprintf(MSG_FORCE, "%s[%s]\n", name, buf);
    free(buf);
    return 0;
}

int archreader_fmt_version(carchreader *ai, void *volhead)
{
    int magiclen;
    
    assert(ai);
    
    // interpret the buffer and get file format version
    magiclen=strlen(FSA_FILEFORMAT);
    if ((memcmp(volhead+40, "FsArCh_001", magiclen)==0) || (memcmp(volhead+40, "FsArCh_00Y", magiclen)==0))
    {
        ai->filefmtver=1;    
    }
    else if (memcmp(volhead+42, "FsArCh_002", magiclen)==0)
    {
        ai->filefmtver=2;
    }
    else
    {
        return -1;
    }

    msgprintf(MSG_VERB2, "Detected fileformat=%d in archive %s\n", (int)ai->filefmtver, ai->volpath);
    
    return 0;
}

int archreader_read_header(carchreader *ai, char *magic, cdico **d, bool readvol, u16 *fsid)
{
    s64 readsize, unread;
    u16 temp16;
    u32 temp32;
    u32 archid;
    int res, i;
    bool leave;
    char volhead[FSA_CACHE_HEADER];
    
    assert(ai);
    assert(d);
    assert(fsid);
    
    // init
    *fsid=FSA_FILESYSID_NULL;
    *d=NULL;
    leave=false;
    readsize=FSA_SIZEOF_MAGIC;
    unread=FSA_CACHE_HEADER;
    memset(magic, 0, FSA_SIZEOF_MAGIC);
            
    while (!(leave||get_abort())) 
    {
        while (!(leave||get_abort()))
        {
            // read a new block to be scaned
            if (archreader_read_data(ai, volhead, readsize)!=FSAERR_SUCCESS)
            {   errprintf("end of archive found while searching for a magic\n");
                return OLDERR_FATAL;
            }
            // test each byte of the block for a valid magic
            for (i=0; !leave && (i<=readsize-FSA_SIZEOF_MAGIC); i++)
            {
                leave=is_magic_valid(&volhead[i]);
            }
            // unread the remaining bytes to be re-scaned
            if (archreader_unread(ai, leave?readsize-i+1:FSA_SIZEOF_MAGIC-1)!=FSAERR_SUCCESS)
            {   errprintf("error unreading the magic data\n");
                return OLDERR_FATAL;
            }
            // grow the read size in case we need to iterate more
            readsize=FSA_CACHE_HEADER;            
        }
        
        if (get_abort())
        {   errprintf("operation aborted by user request\n");
            return OLDERR_FATAL;
        }
        
        // interpret headervol and get file format version
        if (readvol)
        {   // read file format version
            if (archreader_read_data(ai, volhead, FSA_CACHE_HEADER)!=FSAERR_SUCCESS)
            {   errprintf("cannot read the volume magic from %s\n", ai->volpath);
                msgprintf(MSG_STACK, "%s is not a supported fsarchiver file format\n", ai->volpath);                
                return OLDERR_FATAL;
            }
            // check the file format version
            if (archreader_fmt_version(ai, volhead)!=FSAERR_SUCCESS)
            {   // we haven't found a good archive time to continue
                unread=FSA_CACHE_HEADER-FSA_SIZEOF_MAGIC;
                leave=false;
            }
            // unread the remaining bytes to be re-scaned
            if (archreader_unread(ai, unread)!=FSAERR_SUCCESS)
            {   errprintf("error unreading the volume header data: unread(%ld) failed\n", unread);
                return OLDERR_FATAL;     
            }
        }
    }
    
    if (get_abort())
    {   errprintf("operation aborted by user request\n");
        return OLDERR_FATAL;
    }
    
    if (archreader_read_data(ai, magic, FSA_SIZEOF_MAGIC)!=FSAERR_SUCCESS)
    {   errprintf("cannot read header magic\n");                
        return OLDERR_FATAL;
    }
    
    if ((*d=dico_alloc())==NULL)
    {   errprintf("dico_alloc() failed\n");
        return OLDERR_FATAL;
    }

    // read the archive id
    if ((res=archreader_read_data(ai, &temp32, sizeof(temp32)))!=FSAERR_SUCCESS)
    {   errprintf("cannot read archive-id in header: res=%d\n", res);
        return OLDERR_FATAL;
    }
    archid=le32_to_cpu(temp32);
    if (ai->archid) // only check archive-id if it's known (when main header has been read)
    {
        if (archid!=ai->archid)
        {   errprintf("archive-id in header does not match: archid=[%.8x], expected=[%.8x]\n", archid, ai->archid);
            return OLDERR_MINOR;
        }
    }
    
    // read the filesystem id
    if ((res=archreader_read_data(ai, &temp16, sizeof(temp16)))!=FSAERR_SUCCESS)
    {   errprintf("cannot read filesystem-id in header: res=%d\n", res);
        return OLDERR_FATAL;
    }
    *fsid=le16_to_cpu(temp16);
    
    // read the dico of the header
    if ((res=archreader_read_dico(ai, *d))!=FSAERR_SUCCESS)
    {   errprintf("imgdisk_read_dico() failed\n");
        return res;
    }
    
    return FSAERR_SUCCESS;
}

int archreader_read_volheader(carchreader *ai)
{
    char creatver[FSA_MAX_PROGVERLEN];
    char filefmt[FSA_MAX_FILEFMTLEN];
    char magic[FSA_SIZEOF_MAGIC];
    cdico *d;
    u32 volnum;
    u32 readid;
    u16 fsid;
    int res;
    int ret=0;
    
    // init
    assert(ai);
    memset(magic, 0, sizeof(magic));

    // ---- a. read header from archive file
    if ((res=archreader_read_header(ai, magic, &d, true, &fsid))!=FSAERR_SUCCESS)
    {   errprintf("archreader_read_header() failed to read the archive header\n");
        return -1;
    }
    
    // ---- b. check the magic is what we expected
    if (strncmp(magic, FSA_MAGIC_VOLH, FSA_SIZEOF_MAGIC)!=0)
    {   errprintf("magic is not what we expected: found=[%s] and expected=[%s]\n", magic, FSA_MAGIC_VOLH);
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (dico_get_u32(d, 0, VOLUMEHEADKEY_ARCHID, &readid)!=0)
    {   errprintf("cannot get VOLUMEHEADKEY_ARCHID from the volume header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    
    // ---- c. check the archive id
    if (ai->archid==0) // archid not know: this is the first volume
    {
        ai->archid=readid;
    }
    else if (readid!=ai->archid) // archid known: not the first volume
    {   errprintf("wrong header id: found=%.8x and expected=%.8x\n", readid, ai->archid);
        ret=-1; goto archio_read_volheader_error;
    }
    
    // ---- d. check the volnum
    if (dico_get_u32(d, 0, VOLUMEHEADKEY_VOLNUM, &volnum)!=0)
    {   errprintf("cannot get VOLUMEHEADKEY_VOLNUM from the volume header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    if (volnum!=ai->curvol) // not the right volume number
    {   errprintf("wrong volume number in [%s]: volnum is %d and we need volnum %d\n", ai->volpath, (int)volnum, (int)ai->curvol);
        ret=-1; goto archio_read_volheader_error;
    }
    
    // ---- d. check the the file format
    if (dico_get_data(d, 0, VOLUMEHEADKEY_FILEFORMATVER, filefmt, FSA_MAX_FILEFMTLEN, NULL)!=0)
    {   errprintf("cannot find VOLUMEHEADKEY_FILEFORMATVER in main-header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (ai->filefmt[0]==0) // filefmt not know: this is the first volume
    {
        memcpy(ai->filefmt, filefmt, FSA_MAX_FILEFMTLEN);
    }
    else if (strncmp(filefmt, ai->filefmt, FSA_MAX_FILEFMTLEN)!=0)
    {
        errprintf("This archive is based on a different file format: [%s]. Cannot continue.\n", ai->filefmt);
        errprintf("It has been created with fsarchiver [%s], you should extrat the archive using that version.\n", ai->creatver);
        errprintf("The current version of the program is [%s], and it's based on format [%s]\n", FSA_VERSION, FSA_FILEFORMAT);
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (dico_get_data(d, 0, VOLUMEHEADKEY_PROGVERCREAT, creatver, FSA_MAX_PROGVERLEN, NULL)!=0)
    {   errprintf("cannot find VOLUMEHEADKEY_PROGVERCREAT in main-header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (ai->creatver[0]==0)
        memcpy(ai->creatver, creatver, FSA_MAX_PROGVERLEN);
    
archio_read_volheader_error:
    dico_destroy(d);
    
    return ret;
}

int archreader_read_block(carchreader *ai, cdico *in_blkdico, int in_skipblock, int *out_sumok, struct s_blockinfo *out_blkinfo)
{
    u32 arblockcsumorig;
    u32 arblockcsumcalc;
    u32 curblocksize; // data size
    u64 blockoffset; // offset of the block in the file
    u16 compalgo; // compression algo used
    u16 cryptalgo; // encryption algo used
    u32 finalsize; // compressed  block size
    u32 compsize;
    u8 *buffer;
    
    assert(ai);
    assert(out_sumok);
    assert(in_blkdico);
    assert(out_blkinfo);
    
    // init
    memset(out_blkinfo, 0, sizeof(struct s_blockinfo));
    *out_sumok=-1;
    
    if (dico_get_u64(in_blkdico, 0, BLOCKHEADITEMKEY_BLOCKOFFSET, &blockoffset)!=0)
    {   msgprintf(3, "cannot get blockoffset from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_REALSIZE, &curblocksize)!=0 || curblocksize>FSA_MAX_BLKSIZE)
    {   msgprintf(3, "cannot get blocksize from block-header\n");
        return -1;
    }
    
    if (dico_get_u16(in_blkdico, 0, BLOCKHEADITEMKEY_COMPRESSALGO, &compalgo)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPRESSALGO from block-header\n");
        return -1;
    }
    
    if (dico_get_u16(in_blkdico, 0, BLOCKHEADITEMKEY_ENCRYPTALGO, &cryptalgo)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ENCRYPTALGO from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_ARSIZE, &finalsize)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARSIZE from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_COMPSIZE, &compsize)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPSIZE from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_ARCSUM, &arblockcsumorig)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARCSUM from block-header\n");
        return -1;
    }
    
    if (in_skipblock==true) // the main thread does not need that block (block belongs to a filesys we want to skip)
    {
        if (ai->skip(ai, (long)finalsize)<0)
        {   sysprintf("cannot skip block (finalsize=%ld) failed\n", (long)finalsize);
            return -1;
        }
        return 0;
    }
    
    // ---- allocate memory
    if ((buffer=malloc(finalsize))==NULL)
    {   errprintf("cannot allocate block: malloc(%d) failed\n", finalsize);
        return FSAERR_ENOMEM;
    }
    
    if (archreader_read_data(ai, buffer, (long)finalsize)!=FSAERR_SUCCESS)
    {   sysprintf("cannot read block (finalsize=%ld) failed\n", (long)finalsize);
        free(buffer);
        return -1;
    }
    
    // prepare blkinfo
    out_blkinfo->blkdata=(char*)buffer;
    out_blkinfo->blkrealsize=curblocksize;
    out_blkinfo->blkoffset=blockoffset;
    out_blkinfo->blkarcsum=arblockcsumorig;
    out_blkinfo->blkcompalgo=compalgo;
    out_blkinfo->blkcryptalgo=cryptalgo;
    out_blkinfo->blkarsize=finalsize;
    out_blkinfo->blkcompsize=compsize;
    
    // ---- checksum
    arblockcsumcalc=fletcher32(buffer, finalsize);
    if (arblockcsumcalc!=arblockcsumorig) // bad checksum
    {
        errprintf("block is corrupt at offset=%ld, blksize=%ld\n", (long)blockoffset, (long)curblocksize);
        *out_sumok=false;
        // if we are a pipe cache the corrupted data to re-scan it again
        if (archreader_unread(ai, out_blkinfo->blkarsize)!=0)
        {
            sysprintf("archreader_unread() failed\n");
            return FSAERR_ENOMEM;
        }
        // reset the block so we dont forward corrupted data
        if (archreader_reset_block(ai, out_blkinfo, out_sumok)!=0)
        {
            sysprintf("archreader_reset_block() failed\n");
            return FSAERR_ENOMEM;
        }
    }
    else // no corruption detected
    {
        *out_sumok=true;
    }
    
    return 0;
}

int archreader_unread(carchreader *ai, s64 size)
{
    s64 offset;
    
    assert(ai);

    if (ai->cache>(ai->cacheread-size))
    {   errprintf("requested to unread more data than we have: %ld > %ld\n", size, ai->cacheread-ai->cache);
        return -1;
    }
 
    ai->cacheread-=size;
    offset=ai->cacheread-ai->cache;
    
    if (offset)
    {
        memmove(ai->cache, ai->cacheread, ai->cachewrite-ai->cacheread);
        ai->cachewrite-=offset;
        ai->cacheread=ai->cache;
    }

    return 0;
}

int archreader_reset_block(carchreader *ai, struct s_blockinfo *out_blkinfo, int *out_sumok) 
{
    assert(ai);

    // reset the block to avoid forwarding corrupted data
    free(out_blkinfo->blkdata);
    
    if ((out_blkinfo->blkdata=malloc(out_blkinfo->blkarsize))==NULL)
    {   errprintf("cannot allocate block: malloc(%d) failed\n", out_blkinfo->blkarsize);
        return FSAERR_ENOMEM;
    }
    
    memset(out_blkinfo->blkdata, 0, out_blkinfo->blkarsize);
    
    return 0;
}

s64 archreader_read_blocks_raw(carchreader *ai, int fd, void *buf, u64 size)
{
    long lres;
    
    assert(ai);
    assert((size%ai->devblocksize)==0);
    
    if ((lres=read(fd, buf, size))<0)
    {   errprintf("cannot read blocks: read(%ld)=%ld failed\n", size, lres);
        return -1;
    }
    
    return lres;
}

s64 archreader_read_select_raw(carchreader *ai, int fd, void *buf, u64 size)
{
    fd_set rfds;
    long lres;
    s64 pending;

    assert(ai);
    
    for (pending=size; (pending>0) && !get_abort(); ) 
    { 
        if (!ai->polling)
        {
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            if ((lres=select(1, &rfds, NULL, NULL, NULL))==-1)
            {   errprintf("cannot select fd: select(%d)=%ld failed\n", fd, lres);
                return -1;
            }
        }
        if ((lres=read(fd, buf, pending))<0)
        {
            if (lres!=-1)
            {   errprintf("cannot read data: read(%ld)=%ld failed\n", pending, lres);
                return -1;
            }
            else if ((errno==EAGAIN)||(errno==EWOULDBLOCK))
            {
                continue;
            }
        }
        else if (lres==0)
        {   // eof
            break;
        }
        else if (lres)
        {
            buf+=lres;
            pending-=lres;
        }  
    }

    if (get_abort())
    {   errprintf("operation aborted by user request\n");
        return -1;
    }
        
    return size-pending;
}

int archreader_precache(carchreader *ai, s64 size)
{
    u8* victim;
    s64 offsetread, offsetwrite, xmod;
    
    assert(ai);
    
    if ((ai->cachewrite+size)<=(ai->cache+ai->cachesize))
    {   // there is enough room for the new data
        return 0;
    }
    
    // grow the buffer
    victim=ai->cache;
    offsetread=ai->cacheread-ai->cache;
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
    
    ai->cacheread=ai->cache+offsetread;
    ai->cachewrite=ai->cache+offsetwrite;
    msgprintf(MSG_DEBUG2,"cache buffer at: %d\n",ai->cachesize);
    
    return 0;
}

int archreader_read_select(carchreader *ai, u64 size)
{
    long lres;
    s64 msize;
    
    assert(ai);
 
    msize=size-(ai->cachewrite-ai->cacheread); 
    if (msize<=0)
    {   // there is enough cached data
        return 0;
    }
    
    if (archreader_precache(ai, msize)!=FSAERR_SUCCESS)
    {   errprintf("precaching error: archreader_precache(%ld) failed\n", msize);
        return -1;
    }
    
    if ((lres=archreader_read_select_raw(ai, ai->archfd, ai->cachewrite, msize))!=msize)
    {   errprintf("cannot read data: archreader_read_select_raw(%ld)=%ld\n", msize, lres);
        return -1;        
    }

    ai->cachewrite+=lres;

    return 0;
}

int archreader_read_blocks(carchreader *ai, u64 size)
{
    long lres;
    s64 msize,xmod;
    
    assert(ai);

    msize=size-(ai->cachewrite-ai->cacheread);
    if (msize<=0)
    {   // there is enough cached data
        return 0;
    }

    xmod=msize%ai->devblocksize;
    msize=xmod==0?msize:msize+(ai->devblocksize-xmod);
    if (archreader_precache(ai, msize)!=FSAERR_SUCCESS)
    {   errprintf("precaching error: archreader_precache(%ld) failed\n", msize);
        return -1;
    }
    
    if ((lres=archreader_read_blocks_raw(ai, ai->archfd, ai->cachewrite, msize))<size)
    {   errprintf("cannot read blocks: archreader_read_blocks_raw(%ld)=%ld failed\n", msize, lres);
        return -1;
    }

    ai->cachewrite+=lres;

    return 0;
}

int archreader_read_regular(carchreader *ai, u64 size)
{
    long lres;
    s64 msize;
    
    assert(ai);

    msize=size-(ai->cachewrite-ai->cacheread);
    if (msize<=0)
    {   // there is enough cached data
        return 0;
    }
    
    if (archreader_precache(ai, msize)!=FSAERR_SUCCESS)
    {   errprintf("precaching error: archreader_precache(%ld) failed\n", msize);
        return -1;
    }

    if ((lres=read(ai->archfd, ai->cachewrite, msize))!=msize)
    {   errprintf("cannot read: read(%ld)=%ld failed\n", msize, lres);
        return -1;
    }
    
    ai->cachewrite+=lres;
    
    return 0;
}

int archreader_skip_regular(carchreader *ai, s64 offset)
{
    long lres;
    s64 moffset;
    
    assert(ai);

    moffset=ai->cachewrite-ai->cacheread;
    if (offset<=moffset)
    {   // there is enough cached data
        ai->cacheread+=offset;
        return 0;
    }

    if ((lres=lseek64(ai->archfd, offset-moffset, SEEK_CUR))<0)
    {   errprintf("cannot seek forward: lseek64(%ld)=%ld failed\n", offset, lres);
        return -1;
    }
    
    ai->cacheread=ai->cachewrite=ai->cache;
    
    return 0;
}

int archreader_skip_select(carchreader *ai, s64 offset)
{
    long lres;
    s64 moffset, pending;
    
    assert(ai);
    
    moffset=ai->cachewrite-ai->cacheread;
    if (offset<=moffset)
    {   // there is enough cached data
        ai->cacheread+=offset;
        return 0;
    }

    for (pending=offset-moffset; (pending>=ai->cachesize) && !get_abort(); pending-=ai->cachesize)
    {
        if ((lres=archreader_read_select_raw(ai, ai->archfd, ai->cache, ai->cachesize))!=ai->cachesize)
        {   errprintf("cannot read data: archreader_read_select_raw(%d)=%ld failed\n", ai->cachesize, lres);
            return -1;
        }
    }

    if (get_abort())
    {   errprintf("operation aborted by user request\n");
        return -1;
    }

    if (pending && ((lres=archreader_read_select_raw(ai, ai->archfd, ai->cache, pending))!=pending))
    {   errprintf("cannot read data: archreader_read_select_raw(%ld)=%ld failed\n", pending, lres);
        return -1;
    }
    
    ai->cacheread=ai->cachewrite=ai->cache;

    return 0;
}

int archreader_skip_blocks(carchreader *ai, s64 offset)
{
    long lres;
    s64 moffset, pending, mpending, xmod;

    assert(ai);

    moffset=ai->cachewrite-ai->cacheread;
    if (offset<=moffset)
    {   // there is enough cached data
        ai->cacheread+=offset;
        return 0;
    }

    for (pending=offset-moffset; (pending>=ai->cachesize) && !get_abort(); pending-=ai->cachesize)
    {
        if((lres=archreader_read_blocks_raw(ai, ai->archfd, ai->cache, ai->cachesize))!=ai->cachesize)
        {   errprintf("cannot seek forward: archreader_read_select(%d)=%ld failed\n", ai->cachesize, lres);
            return -1;
        }
    }

    if (get_abort())
    {   errprintf("operation aborted by user request\n");
        return OLDERR_FATAL;
    }

    if (pending)
    {
        lres=0;
        xmod=pending%ai->devblocksize;
        mpending=xmod==0?pending:pending+(ai->devblocksize-xmod);
        if (pending && ((lres=archreader_read_blocks_raw(ai, ai->archfd, ai->cache, mpending))<mpending))
        {   errprintf("cannot seek forward: archreader_read_select(%ld)=%ld failed\n", mpending, lres);
            return -1;
        }
        ai->cachewrite=ai->cache+lres;
        ai->cacheread=ai->cache+pending;
    }
    else
    {
        ai->cacheread=ai->cachewrite=ai->cache;
    }

    return 0;
}

int archreader_skip_st(carchreader *ai, s64 offset)
{
    // TODO: not implemented
    return archreader_skip_blocks(ai, offset);
}
