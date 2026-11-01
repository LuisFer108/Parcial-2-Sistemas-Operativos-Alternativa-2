CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
SRC     = reproductor.c player.c playlist.c ringbuf.c monitor.c
TARGET  = reproductor

all: $(TARGET)
$(TARGET): $(SRC) player.h playlist.h ringbuf.h monitor.h
	$(CC) $(CFLAGS) $(SRC) -o $@

# Detector de carreras de datos (recomendado para la sustentación)
tsan: CFLAGS = -Wall -Wextra -g -O1 -pthread -fsanitize=thread
tsan: clean $(TARGET)

clean:
	rm -f $(TARGET)
.PHONY: all tsan clean
