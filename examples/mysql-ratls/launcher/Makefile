# Makefile for MySQL RA-TLS Launcher
# This launcher runs inside a Gramine SGX enclave

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lcurl

TARGET = mysql-ratls-launcher
SRCS = mysql_ratls_launcher.c cJSON.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

mysql_ratls_launcher.o: mysql_ratls_launcher.c cJSON.h
cJSON.o: cJSON.c cJSON.h

clean:
	rm -f $(TARGET) $(OBJS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
