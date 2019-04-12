/* See LICENSE file for copyright and license details. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "config.h"

static const char common_shader_header[] =
    "#version 100\n"
    "precision highp float;";

static const char vertex_shader_body[] =
    "attribute vec4 iPosition;"
    "void main(){gl_Position=iPosition;}";

static const char fragment_shader_header[] =
    "uniform vec3 iResolution;"
    "uniform float iTime;"
    "uniform float iFrame;"
    "uniform float iChannelTime[4];"
    "uniform vec4 iMouse;"
    "uniform vec4 iDate;"
    "uniform float iSampleRate;"
    "uniform vec3 iChannelResolution[4];"
    "uniform sampler2D iChannel0;"
    "uniform sampler2D iChannel1;"
    "uniform sampler2D iChannel2;"
    "uniform sampler2D iChannel3;\n";

static const char fragment_shader_footer[] =
    "\nvoid main(){mainImage(gl_FragColor,gl_FragCoord.xy);}";

static Display *x_display;
static Window x_root;
static Window x_window;
static XComposeStatus x_kstatus;
static EGLDisplay egl_display;
static EGLContext egl_context;
static EGLSurface egl_surface;
static GLsizei viewport_width = -1;
static GLsizei viewport_height = -1;
static GLuint shader_program;
static GLint attrib_position;
static GLint sampler_channel[4];
static GLint uniform_cres;
static GLint uniform_frame;
static GLint uniform_ctime;
static GLint uniform_date;
static GLint uniform_gtime;
static GLint uniform_mouse;
static GLint uniform_res;
static GLint uniform_srate;
static int frames = 0;

static void die(const char *format, ...){
    va_list args;

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    exit(EXIT_FAILURE);
}

static void info(const char *format, ...) {
    va_list args;

    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

static double timespec_diff(const struct timespec *start, const struct timespec *stop){
    struct timespec d;
    if ((stop->tv_nsec - start->tv_nsec) < 0){
        d.tv_sec = stop->tv_sec - start->tv_sec - 1;
        d.tv_nsec = stop->tv_nsec - start->tv_nsec + 1000000000l;
    }
    else {
        d.tv_sec = stop->tv_sec - start->tv_sec;
        d.tv_nsec = stop->tv_nsec - start->tv_nsec;
    }

    return (double)d.tv_sec + (double)d.tv_nsec / 1000000000.0;
}

static void monotonic_time(struct timespec *tp){
    if (clock_gettime(CLOCK_MONOTONIC, tp) == -1)
        die("clock_gettime on CLOCK_MONOTIC failed.\n");
}

static GLuint compile_shader(GLenum type, GLsizei nsources, const char **sources){
    GLuint shader;
    GLint success, len;
    GLsizei i, srclens[nsources];
    char *log;

    for (i = 0; i < nsources; ++i)
        srclens[i] = (GLsizei)strlen(sources[i]);

    shader = glCreateShader(type);
    glShaderSource(shader, nsources, sources, srclens);
    glCompileShader(shader);

    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            log = malloc(len);
            glGetShaderInfoLog(shader, len, NULL, log);
            fprintf(stderr, "%s\n\n", log);
            free(log);
        }
        die("Error compiling shader.\n");
    }

    return shader;
}


static void resize_viewport(GLsizei w, GLsizei h){
    if (viewport_width != w || viewport_height != h) {
        glUniform3f(uniform_res, (float)w, (float)h, 0.0f);
        glViewport(0, 0, w, h);
        viewport_width = w;
        viewport_height = h;
        info("Setting window size to (%d,%d).\n", w, h);
    }
}

static void startup(int width, int height, bool fullscreen)
{
    static const EGLint cv[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    int screen, nvi;
    XWindowAttributes gwa;
    XSetWindowAttributes swa;
    XVisualInfo *vi, vit;
    EGLint vid, ncfg, len, success;
    EGLConfig cfg;
    GLuint vtx, frag;
    const char *sources[4];
    char* log;

    if (!(x_display = XOpenDisplay(NULL)))
        die("Unable to open X display.\n");

    if ((egl_display = eglGetDisplay(x_display)) == EGL_NO_DISPLAY)
        die("Unable to get EGL display.\n");

    if (!eglBindAPI(EGL_OPENGL_ES_API))
        die("Unable to bind OpenGL ES API to EGL.\n");

    if (!eglInitialize(egl_display, NULL, NULL))
        die("Unable to initialize EGL.\n");

    if (!eglChooseConfig(egl_display, egl_config, &cfg, 1, &ncfg))
        die("Unable to find EGL framebuffer configuration.\n");

    egl_context = eglCreateContext(egl_display, cfg, EGL_NO_CONTEXT, cv);
    if (egl_context == EGL_NO_CONTEXT)
        die("Unable to create EGL context.\n");

    if (!eglGetConfigAttrib(egl_display, cfg, EGL_NATIVE_VISUAL_ID, &vid))
        die("Unable to get X VisualID.\n");

    screen = DefaultScreen(x_display);
    x_root = RootWindow(x_display, screen);

    vit.visualid = vid;
    if (!(vi = XGetVisualInfo(x_display, VisualIDMask, &vit, &nvi)))
        die("Unable to find matching XVisualInfo for framebuffer.\n");

    swa.background_pixel = 0;
    swa.colormap = XCreateColormap(x_display, x_root, vi->visual, AllocNone);
    swa.event_mask =
        ExposureMask | StructureNotifyMask |
        KeyPressMask | PointerMotionMask;
    swa.override_redirect = False;

    int window_width = width;
    int window_height = height;
    
    if(fullscreen) {
        window_width = DisplayWidth(x_display, screen);
        window_height = DisplayHeight(x_display, screen);
        //Setting swa.override_redirect to True would result a "real" fullscreen window
        //BUT: On multiscreen systems it would stretch over all displays 
        //AND: Keyboard commands aren't processed anymore
        //MAYBE: Switch to glfw or something similar to create the window. As a neat side effect the app would run on Windows too.
        //swa.override_redirect = True;
    }

    x_window = XCreateWindow(x_display, x_root, 0, 0,
            window_width,
            window_height,
            0, vi->depth, InputOutput, vi->visual,
            CWBackPixel | CWColormap | CWEventMask |
            CWOverrideRedirect, &swa);

    XStoreName(x_display, x_window, "esshader");
    XMapWindow(x_display, x_window);
    XFlush(x_display);

    egl_surface = eglCreateWindowSurface(egl_display, cfg, x_window, NULL);
    if (egl_surface == EGL_NO_SURFACE)
        die("Unable to create EGL window surface.\n");

    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);

    sources[0] = common_shader_header;
    sources[1] = vertex_shader_body;
    vtx = compile_shader(GL_VERTEX_SHADER, 2, sources);

    sources[0] = common_shader_header;
    sources[1] = fragment_shader_header;
    sources[2] = default_fragment_shader;
    sources[3] = fragment_shader_footer;
    frag = compile_shader(GL_FRAGMENT_SHADER, 4, sources);

    shader_program = glCreateProgram();
    glAttachShader(shader_program, vtx);
    glAttachShader(shader_program, frag);
    glLinkProgram(shader_program);

    glGetProgramiv(shader_program, GL_LINK_STATUS, &success);
    if (!success) {
        glGetProgramiv(shader_program, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            log = malloc(len);
            glGetProgramInfoLog(shader_program, len, &len, log);
            fprintf(stderr, "%s\n\n", log);
            free(log);
        }
        die("Error linking shader program.\n");
    }

    glDeleteShader(vtx);
    glDeleteShader(frag);
    glReleaseShaderCompiler();

    glUseProgram(shader_program);
    glValidateProgram(shader_program);

    attrib_position = glGetAttribLocation(shader_program, "iPosition");
    sampler_channel[0] = glGetUniformLocation(shader_program, "iChannel0");
    sampler_channel[1] = glGetUniformLocation(shader_program, "iChannel1");
    sampler_channel[2] = glGetUniformLocation(shader_program, "iChannel2");
    sampler_channel[3] = glGetUniformLocation(shader_program, "iChannel3");
    uniform_cres = glGetUniformLocation(shader_program, "iChannelResolution");
    uniform_frame = glGetUniformLocation(shader_program, "iFrame");
    uniform_ctime = glGetUniformLocation(shader_program, "iChannelTime");
    uniform_date = glGetUniformLocation(shader_program, "iDate");
    uniform_gtime = glGetUniformLocation(shader_program, "iTime");
    uniform_mouse = glGetUniformLocation(shader_program, "iMouse");
    uniform_res = glGetUniformLocation(shader_program, "iResolution");
    uniform_srate = glGetUniformLocation(shader_program, "iSampleRate");

    if (!XGetWindowAttributes(x_display, x_window, &gwa))
        die("Unable to get window size.\n");

    resize_viewport(gwa.width, gwa.height);
}

static void shutdown(void){
    glDeleteProgram(shader_program);
    eglDestroyContext(egl_display, egl_context);
    eglDestroySurface(egl_display, egl_surface);
    eglTerminate(egl_display);
    XDestroyWindow(x_display, x_window);
    XCloseDisplay(x_display);
}

static bool process_event(XEvent *ev){
    char kbuf[32];
    KeySym key;

    switch (ev->type) {
        case ConfigureNotify:
            resize_viewport(ev->xconfigure.width, ev->xconfigure.height);
            break;
        case KeyPress:
            XLookupString(&ev->xkey, kbuf, sizeof(kbuf), &key, &x_kstatus);
            if (key == XK_Escape || key == XK_q)
                return false;
            break;
        default:
            break;
    }

    return true;
}

static bool process_events(void){
    bool done = false;
    XEvent ev;

    while (XPending(x_display)) {
        XNextEvent(x_display, &ev);
        if (!process_event(&ev)) {
            done = true;
        }
    }

    return !done;
}

static void render(float abstime){
    static const GLfloat vertices[] = {
        -1.0f, -1.0f,
        1.0f, -1.0f,
        -1.0f, 1.0f,
        1.0f, 1.0f,
    };

    if(uniform_gtime >= 0)
        glUniform1f(uniform_gtime, abstime);

    glUniform1f(uniform_frame, (float)(++frames));

    glClearColor(0.0f, 0.0f, 0.0f, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableVertexAttribArray(attrib_position);
    glVertexAttribPointer(attrib_position, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    eglSwapBuffers(egl_display, egl_surface);
}

//Reads a file into a string
//Return string or NULL on failure
static char* read_file_into_str(const char *filename) {
    long length = 0;
    char *result = NULL;
    FILE *file = fopen(filename, "r");
    if(file) {
        int status = fseek(file, 0, SEEK_END);
        if(status != 0) {
            fclose(file);
            return NULL;
        }
        length = ftell(file);
        status = fseek(file, 0, SEEK_SET);
        if(status != 0) {
            fclose(file);
            return NULL;
        }
        result = malloc((length+1) * sizeof(char));
        if(result) {
            size_t actual_length = fread(result, sizeof(char), length , file);
            result[actual_length++] = '\0';
        } 
        fclose(file);
        return result;
    }
    return NULL;
}

int main(int argc, char **argv){
    info("ESShader -  Version: %s\n", VERSION);

    struct timespec start, cur;
    
    //Default selected_options
    bool fullscreen = false;
    int window_width = 640;
    int window_height = 360;

    int temp_width = 0;
    int temp_height = 0;

    //shader program
    char *program_source = NULL;

    //Parse command line selected_options
    int selected_option = -1;
    int selected_index = 0;
    while((selected_option = getopt_long (argc, argv, options_string, long_options, &selected_index)) != -1) {
    switch(selected_option) {
        case 'f':
            fullscreen = true;
            break;
        case 'w':
            temp_width = atoi(optarg);
            if(temp_width > 0) {
                window_width = temp_width;
            }
            break;
        case 'h':
            temp_height = atoi(optarg);
            if(temp_height > 0) {
                window_height = temp_height;
            }
            break;
        case 's':
            info("Loading shader program: %s\n", optarg);
            program_source = read_file_into_str(optarg);
            if(program_source == NULL) {
                die("Could not read shader program %s\n", optarg);
            }
            default_fragment_shader = program_source;
            break;
        case '?':
            info(   "\nUsage: esshader [OPTIONS]\n"
                    "Example: esshader --width 1280 --height 720\n\n"
                    "Options:\n"
                    " -f, --fullscreen \truns the program in (fake) fullscreen mode.\n"
                    " -?, --help \t\tshows this help.\n"
                    " -w, --width [value] \tsets the window width to [value].\n"
                    " -h, --height [value] \tsets the window height to [value].\n"
                    " -s, --source [path] \tpath to shader program\n"
                    );
            return 0;
        }
    }

    info("Press [ESC] or [q] to exit.\n");
    info("Run with --help flag for more information.\n\n");
    startup(window_width, window_height, fullscreen);
    monotonic_time(&start);

    for (;;) {
        if (!process_events()) {
            break;
        }
        render((float)timespec_diff(&start, &cur));
        monotonic_time(&cur);
    }

    shutdown();
    if(program_source != NULL) {
        free(program_source);
        program_source = NULL;
    }
    return 0;
}

