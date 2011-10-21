#!/bin/bash

# test_coverage.sh test fsarchiver using different devices and corrupted data
#
# Copyright (C) 2011-onwards the fsarchiver-kr team. All rights reserved.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public
# License v2 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.

set -e
set -u

BLOCK=4096
FSA="../src/fsarchiver"
OPTIONS="-j2" # -dd -vv
FS1="fs1.loop"
FS2="fs2.loop"
FS3="fs3.loop"
ORIGINAL="original.loop"
DESTINY="destiny.loop"
BUFFER="buffer.loop"
LOOPS="$FS1 $FS2 $FS3 $ORIGINAL $DESTINY $BUFFER"
CORRUPTED_FILES=""
ORIGINAL_DIR="original"
TMP_DIR="tmp"
FSA_FILE="fsa_test_fs.fsa"
ORIGINAL_DAT="original.dat"
ORIGINAL_FSA="original.fsa"
DEPS="socat python rsync sed dd od date losetup mkfifo killall mount umount mknod"
set +u
[ -z "$TAPE" ] && TAPE=""
set -u

find_corrupt_files() {
    set +e
    CORRUPTED_FILES=`find -name 'corrupted_*.fsa' -type f`
    set -e
}

corrupt_files() {
    find_corrupt_files
    if [ -n "$CORRUPTED_FILES" ]; then
        echo ///reusing corrupted files: $CORRUPTED_FILES///
    else
        echo ///corrupting some files... it will take some time///
        local file_size=`stat -c%s "$ORIGINAL_FSA"`
        cat $ORIGINAL_FSA | ./corrupt.py -S 9 -t 4MB -s $file_size -A -B -c 0.0000009 > corrupted_before_heavy_$FSA_FILE
        cat $ORIGINAL_FSA | ./corrupt.py -s $file_size -A -c 0.0000002 > corrupted_before_mild_$FSA_FILE
        find_corrupt_files
    fi
}

make_loops() {
    local minor=128
    for i in $LOOPS; do
        mknod $i b 7 $minor
        set +e
        let minor++
        set -e
    done
}

check_differences() {
    mount -o ro $1 $TMP_DIR
    rsync -vrcn $ORIGINAL_DIR/ $TMP_DIR
    umount $TMP_DIR
}

get_random_name() {
    echo `od -An -N2 /dev/random | sed -e 's/^[ \t]+//'``date +%N`
}

create_file() {
    dd if=/dev/zero of=$1 bs=$BLOCK count=$2 &>/dev/null
}

setup_loop() {
    losetup $1 $2
}

populate_fs() {
    local name=""
    mkreiserfs -q -l $2 $1 &>/dev/null
    mount $1 $2
    i=0
    while [ $i -lt $3 ]; do
        name=$(get_random_name)
        dd if=/dev/urandom of=$2/$name bs=$BLOCK count=$4 &>/dev/null
        sha256sum $2/$name > $2/$name.sha256
        set +e
        let i++
        set -e
    done
    mount -o remount,ro $1 $2
}

create_fs() {
    if [ -f "$ORIGINAL_DAT" ]; then
        echo ///reusing file system: $ORIGINAL_DAT///
        setup_loop $ORIGINAL $ORIGINAL_DAT
        mount -o ro $ORIGINAL $ORIGINAL_DIR
    else
        echo ///creating a new file system... it will take some time///
        create_file $1 $2
        setup_loop $ORIGINAL $1
        populate_fs $ORIGINAL $ORIGINAL_DIR $3 $4
    fi
}

create_fsa() {
    if [ -f "$ORIGINAL_FSA" ]; then
        echo ///reusing the fsa file: $ORIGINAL_FSA///
    else
        echo ///creating a new fsa file... it will take some time///
        $FSA $OPTIONS -o savefs $ORIGINAL_FSA $FS1 $FS2 &>/dev/null
    fi
}

test_stdio() {
    echo ///test:stdio///
    $FSA $OPTIONS savefs - $FS1 $FS3 |\
    $FSA $OPTIONS restfs - id=1,dest=$DESTINY    
}

test_stdio_corrupt() {
    for i in $CORRUPTED_FILES; do
        echo ///test:stdio_corrupt with:$i///
        set +e
        cat $i | $FSA $OPTIONS restfs - id=1,dest=$DESTINY
        set -e
        check_differences $DESTINY
    done
}

test_file() {
    echo ///test:file///
    $FSA $OPTIONS savefs $FSA_FILE $FS1 $FS2 $FS3
    $FSA $OPTIONS restfs $FSA_FILE id=1,dest=$DESTINY
}

test_file_corrupt() {
    for i in $CORRUPTED_FILES; do
        echo ///test:file_corrupt with:$i///
        set +e
        $FSA $OPTIONS restfs $i id=1,dest=$DESTINY
        set -e
        check_differences $DESTINY
    done
}

test_block() {
    echo ///test:block///
    $FSA $OPTIONS savefs $BUFFER $FS1 $FS2 $FS3
    $FSA $OPTIONS restfs $BUFFER id=1,dest=$DESTINY
}

test_block_corrupt() {
    for i in $CORRUPTED_FILES; do
        echo ///test:block_corrupt with:$i///
        losetup -d $BUFFER
        sleep 1
        losetup $BUFFER $i
        sleep 1
        set +e
        $FSA $OPTIONS restfs $BUFFER id=1,dest=$DESTINY
        set -e
        check_differences $DESTINY
    done
}

test_pipe() {
    echo ///test:pipe///
    mkfifo pipe1
    sleep 1
    $FSA $OPTIONS savefs pipe1 $FS1 $FS3&
    sleep 1
    $FSA $OPTIONS restfs pipe1 id=1,dest=$DESTINY
}

test_pipe_corrupt() {
    for i in $CORRUPTED_FILES; do
        echo ///test:pipe_corrupt with:$i///
        cat $i > pipe1&
        sleep 1
        set +e
        $FSA $OPTIONS restfs pipe1 id=1,dest=$DESTINY
        set -e
        check_differences $DESTINY
    done
}

test_socket() {
    echo ///test:socket///
    socat UNIX-LISTEN:sock1 UNIX-LISTEN:sock2&
    sleep 2
    $FSA $OPTIONS savefs sock1 $FS1 $FS3&
    sleep 1
    $FSA $OPTIONS restfs sock2 id=1,dest=$DESTINY
}

test_socket_corrupt() {
    for i in $CORRUPTED_FILES; do
        echo ///test:socket_corrupt with:$i///
        socat FILE:$i,ignoreeof UNIX-LISTEN:sock3&
        sleep 2
        set +e
        $FSA $OPTIONS restfs sock3 id=1,dest=$DESTINY
        set -e
        check_differences $DESTINY
    done
}

test_character() {
    echo ///test:character///
    socat PTY,raw,echo=0,link=pty1,istrip=0 PTY,raw,echo=0,link=pty2,istrip=0&
    sleep 2
    $FSA $OPTIONS savefs pty1 $FS1 $FS3&
    sleep 1
    $FSA $OPTIONS restfs pty2 id=1,dest=$DESTINY
    killall socat
}

test_character_corrupt() {
    for i in $CORRUPTED_FILES; do
        echo ///test:character_corrupt with:$i///
        socat FILE:$i,ignoreeof PTY,raw,echo=0,link=pty3,istrip=0&
        sleep 2
        set +e
        $FSA $OPTIONS restfs pty3 id=1,dest=$DESTINY
        set -e
        killall socat
        check_differences $DESTINY
    done
}

test_tape() {
    echo ///test:tape///
    $FSA $OPTIONS savefs $TAPE $FS1 $FS2 $FS3
    $FSA $OPTIONS restfs $TAPE id=1,dest=$DESTINY
}

test_tape_corrupt() {
    for i in $CORRUPTED_FILES; do
        echo ///test:tape_corrupt with:$i///
		mt defblksize $BLOCK
        dd if=$i of=$TAPE bs=$BLOCK conv=sync &>/dev/null
        set +e
        $FSA $OPTIONS restfs $TAPE id=1,dest=$DESTINY
        set -e
        check_differences $DESTINY
    done
}

test_normal_modes() {
    test_stdio
    test_file
    test_block
    test_character
    test_pipe
    test_socket
    if [ -n "$TAPE" ]; then
        test_tape
    fi
}

test_corrupt_modes() {
    create_fsa
    corrupt_files

    test_stdio_corrupt
    #test_file_corrupt
    #test_block_corrupt
    #test_pipe_corrupt
    #test_character_corrupt
    test_socket_corrupt
    if [ -n "$TAPE" ]; then
        test_tape_corrupt
    fi
}

cleanup() {
    set +e
    killall socat &>/dev/null
    killall -9 fsarchiver &>/dev/null
    sleep 1
    umount $TMP_DIR $ORIGINAL_DIR &>/dev/null
    sleep 1
    losetup -d *.loop &>/dev/null
    sleep 1
    rm fsa_test_*.dat &>/dev/null
    rm fsa_test_*.fsa &>/dev/null
    rm sock* pipe* pty* &>/dev/null
    rm $LOOPS &>/dev/null
    #rm corrupted_*.fsa &>/dev/null
    rmdir $TMP_DIR $ORIGINAL_DIR &>/dev/null
}

handle_sig() {
    exit 1 
}

test_dependencies() {
    set +e
    if [[ "$USER" != "root" ]]; then
        echo "You have to be root to run this test."
        exit 1
    fi
    for i in $DEPS; do
        which $i &>/dev/null
        if [[ $? != 0 ]]; then
            echo "I need $i to run this tests, please install $i."
            exit 1
        fi
    done
    set -e
}

main(){
    local fileblocks=0
    local blocks=0
    local filesize=0
    local filecount
    set +u
    [ -z "$1" ] && filecount=16 || filecount=$1
    [ -z "$2" ] && filesize=1048576 || filesize=$2
    set -u
    let fileblocks=$filesize/$BLOCK
    let blocks=$filecount*$fileblocks*4

    mkdir $TMP_DIR
    mkdir $ORIGINAL_DIR

    make_loops
    create_file fsa_test_destiny.dat $blocks
    create_file fsa_test_buffer.dat $blocks
    create_fs $ORIGINAL_DAT $blocks $filecount $fileblocks

    ln -s $ORIGINAL_DAT fsa_test_fs1.dat
    ln -s $ORIGINAL_DAT fsa_test_fs2.dat
    ln -s $ORIGINAL_DAT fsa_test_fs3.dat

    setup_loop $DESTINY fsa_test_destiny.dat
    setup_loop $BUFFER fsa_test_buffer.dat
    setup_loop $FS1 fsa_test_fs1.dat
    setup_loop $FS2 fsa_test_fs2.dat
    setup_loop $FS3 fsa_test_fs3.dat

    test_normal_modes
    test_corrupt_modes

    echo ///testing finished///

    return 0
}

test_dependencies
trap handle_sig INT TERM
trap cleanup EXIT
main $*
