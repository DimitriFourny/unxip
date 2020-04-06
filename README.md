# unxip

Extract the files from a XIP archive. Works on Linux.

```
$ make
  g++ -O3 unxip.cpp -o unxip -lz -lxml2 -I/usr/include/libxml2/

$ ./unxip ../Xcode_11.4.xip
  Xcode_11.4/Metadata     done
  Xcode_11.4/Content      done

$ file Xcode_11.4/*
  Xcode_11.4/Content:     data (PBZX)
  Xcode_11.4/Metadata:    bzip2 compressed data, block size = 900k
  Xcode_11.4/xpi_toc.xml: XML 1.0 document
```

