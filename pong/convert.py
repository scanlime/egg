#!/usr/bin/env python

import struct


img = open("pong.img", "rb").read()
nWords = len(img) / 4
words = struct.unpack("%dI" % nWords, img)

while words:
  line = words[:5]
  words = words[5:]

  print "   " + " ".join(["0x%08x," % v for v in line]) 
