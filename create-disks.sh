#!/bin/sh

for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
do
    if [[ ! -e disk-$i.img ]]; then
	dd if=/dev/zero of=disk-$i.img bs=1 count=1 seek=1G
    fi
done
