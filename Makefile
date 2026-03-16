CC      = cl
CFLAGS  = /W4 /WX /std:c17 /Zi /nologo
LDFLAGS = /nologo

all: halloc_test.exe bench.exe

halloc_test.exe: halloc.c tests/halloc_test.c
	$(CC) $(CFLAGS) halloc.c tests/halloc_test.c /Fe:halloc_test.exe $(LDFLAGS)

bench.exe: halloc.c bench/bench.c
	$(CC) $(CFLAGS) halloc.c bench/bench.c /Fe:bench.exe $(LDFLAGS)

clean:
	del /Q *.exe *.obj *.pdb 2>nul

.PHONY: all clean