CFLAGS= -std=gnu99
CCOPTIMIZE= -O2

CC=gcc
AR=ar
LD=ld
OBJCOPY=objcopy

sonyc_record: sonyc_record.c
	$(CC) $(CCOPTIMIZE) $< -o $@

clean:
	rm -f sonyc_record audio.pcm24
