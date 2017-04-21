#!/usr/bin/env python
#
# Simple 4-bit RLE compression for the logo. The output is a stream of
# 4-bit nybbles. Runs of 0 or F are encoded as 0 + count or F + count,
# where count is a 4-bit value. Other pixel values never encode runs,
# and are always literal. Counts are encoded as count - 1, so the
# valid run lengths are 1 through 16.
#

raw = open("vmware-logo-16gray-113x17.bin", "rb").read()
out = open("vmware-logo.h", "w")

out.write("#define LOGO_WIDTH 113\n")
out.write("#define LOGO_HEIGHT 17\n")
out.write("unsigned char vmwareLogo[] = {\n")

nybbleCount = 0
run = None

def writeNybble(n):
    global nybbleCount

    if nybbleCount & 1:
        out.write("%x," % n)
    else:
        out.write("0x%x" % n)
    nybbleCount += 1
    if (nybbleCount & 31) == 0:
        out.write("\n")

def flushRun():
    global run
    if run:
        value, count = run
        assert count > 0 and count <= 16
        writeNybble(value)
        writeNybble(count - 1)
        run = None

for byte in raw:
    byte = ord(byte)

    if byte in (0, 0xF):
        if run and byte == run[0] and run[1] < 16:
            run[1] += 1
        else:
            flushRun()
            run = [byte, 1]
    else:
        flushRun()
        writeNybble(byte)

flushRun()
count = nybbleCount
if nybbleCount & 1:
    writeNybble(0)  # Pad
out.write("\n};   // %d nybbles\n" % count);


