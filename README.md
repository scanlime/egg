PongOS Easter Egg (R.I.P.)
--------------------------

Source code for the now defunct easter egg, used to be included in VMware products when booting a zero-length floppy disk image.

You can recreate the experiment by booting from https://github.com/scanlime/egg/blob/master/pong/pong-hosted11.img as a floppy disk image.

Won't quite work on a real PC, on account of using paravirtualized interfaces for video and mouse.

Apparently works fine in QEMU, with both video and mouse!

`qemu-system-i386 -vga vmware -fda pong/pong-hosted11.img`

-m

