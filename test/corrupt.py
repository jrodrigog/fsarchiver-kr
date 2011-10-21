#!/usr/bin/env python

"""
corrupt.py corrupt a fsarchiver archive in different ways

Copyright (C) 2011-onwards the fsarchiver-kr team. All rights reserved.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public
License v2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.
"""

import sys, re, optparse, random, math

DEF_PRESERVE = "64"
DEF_CORRUPT = 0.0
DEF_SPLIT = "0"
DEF_SIZE = "0"
DEF_BLOCK = "1024"
DEF_TRASH = DEF_BLOCK
FSA_MAGIC = "FsA0"

RE_UNIT = re.compile(r"""
(?P<count>\d+)
(?P<mult>[kmg]){0,1}
(?P<byte>b){0,1}""",re.I|re.X)
POWER_UNIT = {
    "k" : 10,
    "m" : 20,
    "g" : 30,
}
written = 0
readed = 0

class UnknownUnit(Exception): pass

def to_unit(data):
    m = RE_UNIT.search(data)
    mult = 1
    if m.group("mult"):
        base = (10,2)[m.group("byte")!=None]
        try:
            mult = base ** POWER_UNIT[m.group("mult").lower()]
        except KeyError:
            raise UnknownUnit("the unit in '%s' is unknown." % data)
        return long(m.group("count")) * mult
    elif m.group("count"):
        return long(m.group("count"))
    raise UnknownUnit("the unit in '%s' is unknown." % data)

def put_err(data):
    sys.stderr.write(data)
    sys.stderr.flush()

def put_data(data):
    global written
    written += len(data)
    sys.stdout.write(data)
    sys.stdout.flush()
    return written

def get_data(size):
    global readed
    data = sys.stdin.read(size)
    readed += len(data)
    return data
    
def do_corruption(data):
    k = random.randrange(0,len(data))
    return "%s%s%s" % ( data[:k], chr((ord(data[k])+1)%256), data[k+1:] )
     
def main():
    global writte, readed
    parser = optparse.OptionParser()
    parser.add_option("-A", "--after",
                    action="store_true", dest="after", default=False,
                    help="insert corrupted data before (only patched fsa)")
    parser.add_option("-B", "--before",
                    action="store_true", dest="before", default=False,
                    help="insert corrupted data after")
    parser.add_option("-p", "--preserve",
                    action="store", dest="preserve", default=DEF_PRESERVE,
                    help="preserve bytes from the header (default %s)" % DEF_PRESERVE )
    parser.add_option("-c", "--corrupt",
                    action="store", dest="corrupt", default=DEF_CORRUPT,
                    help="corruption probability (default %.1f)" % DEF_CORRUPT )
    parser.add_option("-S", "--split",
                    action="store", dest="split", default=DEF_SPLIT,
                    help="split the file in blocks (default %s, no split)" % DEF_SPLIT )
    parser.add_option("-t", "--trash",
                    action="store", dest="trash", default=DEF_TRASH,
                    help="insert trash in between splitted blocks (default %s)" % DEF_TRASH )
    parser.add_option("-s", "--size",
                    action="store", dest="size", default=DEF_SIZE,
                    help="a size hint to split the file better")
    parser.add_option("-b", "--block",
                    action="store", dest="block", default=DEF_BLOCK,
                    help="handle a corrupted block of the size (default: %s)" % DEF_BLOCK)
    parser.add_option("-v", "--verbose",
                    action="store_true", dest="verbose", default=False,
                    help="be more verbose about the operations" )

    (options, args) = parser.parse_args()
    
    block = to_unit(options.block)
    preserve = to_unit(options.preserve)
    trash = to_unit(options.trash)
    size = to_unit(options.size)
    split = to_unit(options.split)
    corruption = open("/dev/urandom","rb")
    try:
        corrupt = float(options.corrupt)
    except ValueError:
        raise RuntimeError("invalid corrupt ratio %s" % options.corrupt)
    if block > trash:
        block = trash
    elif block <= 0:
        block = 1

    split_points = []
    if split > 0:
        split_size = size / split
        for i in xrange(split):
            split_points.append(((i+1) * split_size) + (split_size * (random.random() - 0.5))) 
        split_points.sort()

    corrupt_points=[]
    if corrupt > 0.:
        corrupt_points = [
            random.random() * size
            for i in xrange(int(size*corrupt))]
        corrupt_points.sort()
        
    if options.before:
        put_data(corruption.read(block))
        #put_data(FSA_MAGIC, written)
        put_data(corruption.read(block))

    if preserve > 0:
        data = get_data(preserve)
        put_data(data)

    data = get_data(block)
    while data:
        if corrupt_points and readed >= corrupt_points[0]:
            p = corrupt_points.pop(0)
            if options.verbose:
                put_err("inserting corruption here at: %d\n" % p)
            put_data(do_corruption(data))
        else:
            put_data(data)

        if split_points and readed >= split_points[0]:
            p = split_points.pop(0)
            if options.verbose:
                put_err("inserting split here at: %d\n" % p)
            put_data(corruption.read(trash))

        data = get_data(block)

    if options.after:
        put_data(corruption.read(block))
        #TODO: Insert fake end of vol here
        put_data(corruption.read(block))

if __name__ == "__main__":
    main()
