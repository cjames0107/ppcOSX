/*
 * Print whether the WindowServer composites the main display with OpenGL
 * (Quartz Extreme). Works from 10.2 on, so it also covers Panther, whose
 * system_profiler cannot show it from the command line.
 *
 *   cc -o qecheck qecheck.c -framework ApplicationServices
 */
#include <stdio.h>
#include <ApplicationServices/ApplicationServices.h>

int main(void)
{
    CGDirectDisplayID d = CGMainDisplayID();

    printf("Quartz Extreme: %s\n",
           CGDisplayUsesOpenGLAcceleration(d) ? "yes" : "no");
    return 0;
}
