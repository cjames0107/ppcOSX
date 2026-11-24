# Test programs that use only Apple's frameworks (no ppcosx code).

PROGRAMS = build/glutgears build/gluttex build/glutarrays build/glutspecular build/glutquadrics

all: $(PROGRAMS)

build/%: %.c
	mkdir -p build
	gcc -arch ppc -O2 -Wall $< -framework GLUT -framework OpenGL -o $@

clean:
	rm -rf build
