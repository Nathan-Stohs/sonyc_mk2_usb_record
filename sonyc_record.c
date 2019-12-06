#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <stdint.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <ctype.h>

/*

A quick and dirty reader and de-framer for recording SONYC data from the MKII node
This is NOT a robust serial protocol and is only meant for testing

In particular, marker bytes are not sanitized and protocol not robust to corruption

nathan.stohs@samraksh.com
2019-12-06

*/


#define COMPORT "/dev/ttyACM0"		// Default USB-CDC (if only one) on RPI
#define FILENAME "audio.pcm24"		// Default output audio. 24-bit PCM

#define AUDIO_FRAME_SAMPLES 2000
#define SAMP_SIZE 3			// 3 bytes per sample
#define AUDIO_FRAME_MARKER 0x7F		// 4 of these signal start of audio frame
#define WRITE_MAX_BYTES (256*1024*1024) // 256 MiB, a bit over 45 minutes at 32 kHz 
#define SERIAL_BUF_BYTES 8192		// Large enough for a full frame
#define MY_BAUD_RATE 115200		// fiction for USB-CDC

uint8_t buf[SERIAL_BUF_BYTES]; 		// main serial port buffer
int not_done;				// for SIGINT
uint8_t framebuf[AUDIO_FRAME_SAMPLES*SAMP_SIZE]; // frame buffer

int open_port(void) {
	int fd; /* File descriptor for the port */
	fd = open(COMPORT, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd == -1) {
		perror("open_port: Unable to open "COMPORT);
	}
	else {
		fcntl(fd, F_SETFL, 0); // Set as blocking
		printf("Opened port %s\n", COMPORT);
	}
	return fd;
}

int set_mf_attr(int fd) {
	struct termios options;
	tcgetattr(fd, &options);

	/* set raw input */
	options.c_cflag     |= (CLOCAL | CREAD);
	options.c_lflag     &= ~(ICANON | ECHO | ECHOE | ISIG);
	options.c_oflag     &= ~OPOST;

	// Timeouts... lets not mess with it
	//options.c_cc[VMIN]  = 0;   // Setting this > 0 seems to break cygwin
	//options.c_cc[VTIME] = 255; // no time out

	options.c_iflag &= ~(IXON | IXOFF | IXANY);

	cfsetspeed(&options, MY_BAUD_RATE); // Doesn't do anything
	tcsetattr(fd, TCSANOW, &options);
}

void my_handler(int signum) { not_done = 0; }


int main(int argc, char **argv) {
	int fd;   // USB-CDC
	FILE *fp; // Output audio
	int ret;
	unsigned long long bytes=0;
	not_done = 1;
	unsigned marks = 0;
	unsigned dropped = 0;

	fd = open_port();
	if (fd < 0) return 1;

	fp = fopen(FILENAME, "wb");
	if (fp == NULL) {
		perror("Could not open output file: ");
		goto out;
	}
	
	// Register handler
	if (signal(SIGINT, my_handler) == SIG_ERR) {
		perror("Could not register SIGINT: ");
	}

	set_mf_attr(fd);

	// one loop per frame	
	while(not_done) {
		ssize_t r_got;
		size_t wrote;
		unsigned i;
		unsigned frame_bytes = 0;
		uint8_t *frame_ptr;

		// Looking for marker 
		r_got = read(fd, buf, sizeof(buf));
		if (r_got == 0) break;
		if (r_got < 0) {
			perror("read() error: ");
			break;
		}

		// Check for marker
		for(i=0; i<r_got && marks<4; i++) {
			if (buf[i] == AUDIO_FRAME_MARKER) marks++;
			else marks = 0;
		}

		dropped += i;
		if (marks != 4) continue; // didn't find marker, loop and try again
		
		// A frame was found
		if (i>4)
			printf("Dropped %u bytes\n", i-4);
		frame_ptr = &buf[i];
		dropped = 0;
		marks = 0;
		r_got -= i;

		// Some, none, or all of frame payload in buffer
		// Copy what we have to frame buffer
		if (r_got > 0)
			memcpy(framebuf, frame_ptr, r_got);
		frame_bytes = r_got;

		// loop until frame is finished
		while (frame_bytes < AUDIO_FRAME_SAMPLES*SAMP_SIZE) {
			unsigned rem = AUDIO_FRAME_SAMPLES*SAMP_SIZE - frame_bytes;
			frame_ptr = &framebuf[frame_bytes];
			r_got = read(fd, frame_ptr, rem);
			frame_bytes += r_got;
		}

		// Frame should be complete now

		// Write partial frame if full puts us over max file size
		if (frame_bytes + bytes > WRITE_MAX_BYTES) {
			frame_bytes = WRITE_MAX_BYTES - bytes;
		}

		wrote = fwrite(framebuf, 1, frame_bytes, fp);
		if (wrote != frame_bytes) {
			perror("File write error: ");
			break;
		}

		bytes += frame_bytes;
		if (bytes >= WRITE_MAX_BYTES) break;
	}

out:
	printf("Wrote %llu bytes (%llu MB)\n", bytes, bytes/1024/1024);
	printf("Done!\n");
	close(fd);
	fclose(fp);
	return 0;
}

