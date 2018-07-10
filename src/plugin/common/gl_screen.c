#include "gl_screen.h"
#include "core/msg.h"

#include <stdlib.h>
#include <string.h>

#ifdef GLES
#include <GLES3/gl3.h>
#define SHADER_HEADER "#version 300 es\nprecision lowp float;\n"
#define TEX_FORMAT GL_RGBA
#define TEX_TYPE GL_UNSIGNED_BYTE
#else
#include "gl_core_3_3/gl_core_3_3.c"
#define SHADER_HEADER "#version 330 core\n"
#define TEX_FORMAT GL_BGRA
#define TEX_TYPE GL_UNSIGNED_INT_8_8_8_8_REV
#endif

static GLuint program;
static GLuint vao;
static GLuint texture;
static GLuint depth_texture;

static int32_t tex_width;
static int32_t tex_height;

static int32_t tex_display_height;

#ifdef _DEBUG
static void gl_check_errors(void)
{
    GLenum err;
    static int32_t invalid_op_count = 0;
    while ((err = glGetError()) != GL_NO_ERROR) {
        // if gl_check_errors is called from a thread with no valid
        // GL context, it would be stuck in an infinite loop here, since
        // glGetError itself causes GL_INVALID_OPERATION, so check for a few
        // cycles and abort if there are too many errors of that kind
        if (err == GL_INVALID_OPERATION) {
            if (++invalid_op_count >= 100) {
                msg_error("gl_check_errors: invalid OpenGL context!");
            }
        } else {
            invalid_op_count = 0;
        }

        char* err_str;
        switch (err) {
            case GL_INVALID_OPERATION:
                err_str = "INVALID_OPERATION";
                break;
            case GL_INVALID_ENUM:
                err_str = "INVALID_ENUM";
                break;
            case GL_INVALID_VALUE:
                err_str = "INVALID_VALUE";
                break;
            case GL_OUT_OF_MEMORY:
                err_str = "OUT_OF_MEMORY";
                break;
            case GL_INVALID_FRAMEBUFFER_OPERATION:
                err_str = "INVALID_FRAMEBUFFER_OPERATION";
                break;
            default:
                err_str = "unknown";
        }
        msg_debug("gl_check_errors: %d (%s)", err, err_str);
    }
}
#else
#define gl_check_errors(...)
#endif

static GLuint gl_shader_compile(GLenum type, const GLchar* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint param;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &param);

    if (!param) {
        GLchar log[4096];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        msg_error("%s shader error: %s\n", type == GL_FRAGMENT_SHADER ? "Frag" : "Vert", log);
    }

    return shader;
}

static GLuint gl_shader_link(GLuint vert, GLuint frag)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    GLint param;
    glGetProgramiv(program, GL_LINK_STATUS, &param);

    if (!param) {
        GLchar log[4096];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        msg_error("Shader link error: %s\n", log);
    }

    glDeleteShader(frag);
    glDeleteShader(vert);

    return program;
}

void gl_screen_init(struct rdp_config* config)
{
#ifndef GLES
    // load OpenGL function pointers
    ogl_LoadFunctions();
#endif

    msg_debug("%s: GL_VERSION='%s'", __FUNCTION__, glGetString(GL_VERSION));
    msg_debug("%s: GL_VENDOR='%s'", __FUNCTION__, glGetString(GL_VENDOR));
    msg_debug("%s: GL_RENDERER='%s'", __FUNCTION__, glGetString(GL_RENDERER));
    msg_debug("%s: GL_SHADING_LANGUAGE_VERSION='%s'", __FUNCTION__, glGetString(GL_SHADING_LANGUAGE_VERSION));

    // shader sources for drawing a clipped full-screen triangle. the geometry
    // is defined by the vertex ID, so a VBO is not required.
    const GLchar* vert_shader =
        SHADER_HEADER
        "out vec2 uv;\n"
        "void main(void) {\n"
        "    uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n"
        "}\n";

    const GLchar* frag_shader =
        SHADER_HEADER
        "in vec2 uv;\n"
        "layout(location = 0) out vec4 color;\n"
        "uniform sampler2D tex0;\n"
        "uniform sampler2D tex1;\n"
    	"float UnpackDepth(in vec3 pack)\n"
	"{\n"
		"float depth = dot( pack, 1.0 / vec3(1.0, 256.0, 256.0*256.0) );\n"
	  	"return depth * (256.0*256.0*256.0) / (256.0*256.0*256.0 - 1.0);\n"
	"}\n"
        "void main(void) {\n"
#ifdef GLES
        "    color = texture(tex0, uv);\n"
#else
        "    color.bgra = texture(tex0, uv);\n"
#endif
        "    gl_FragDepth = UnpackDepth(texture(tex1, uv).rgb);\n"
        "}\n";

    // compile and link OpenGL program
    GLuint vert = gl_shader_compile(GL_VERTEX_SHADER, vert_shader);
    GLuint frag = gl_shader_compile(GL_FRAGMENT_SHADER, frag_shader);
    program = gl_shader_link(vert, frag);
    glUseProgram(program);

    // prepare dummy VAO
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // prepare depth texture
    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &depth_texture);
    glBindTexture(GL_TEXTURE_2D, depth_texture);
    
    // prepare texture
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    // select interpolation method
    GLint filter;
    switch (config->vi.interp) {
        case VI_INTERP_LINEAR:
            filter = GL_LINEAR;
            break;
        case VI_INTERP_NEAREST:
        default:
            filter = GL_NEAREST;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // check if there was an error when using any of the commands above
    gl_check_errors();
}

bool gl_screen_write(struct rdp_frame_buffer* fb, int32_t output_height)
{
    bool buffer_size_changed = tex_width != fb->width || tex_height != fb->height;
    
    // check if the framebuffer size has changed
    if (buffer_size_changed) {
        tex_width = fb->width;
        tex_height = fb->height;

	// write the depth to the depthbuffer
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, depth_texture);
	msg_debug("%s: attempted to attribute depth: %d", __FUNCTION__, fb->depth);

	// set pitch for all unpacking operations
	glPixelStorei(GL_UNPACK_ROW_LENGTH, fb->pitch);
	    
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, fb->width, fb->height, 0, GL_DEPTH_COMPONENT, TEX_TYPE, fb->depth);

	// switch back to the default texture
	glDisable(GL_DEPTH_TEST);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);

	glDepthMask(GL_FALSE);
	    
        // set pitch for all unpacking operations
        glPixelStorei(GL_UNPACK_ROW_LENGTH, fb->pitch);

        // reallocate texture buffer on GPU
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_width, tex_height, 0, TEX_FORMAT, TEX_TYPE, fb->pixels);

        msg_debug("%s: resized framebuffer texture: %dx%d", __FUNCTION__, tex_width, tex_height);
    } else {

        // copy local buffer to GPU texture buffer
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_width, tex_height,
            TEX_FORMAT, TEX_TYPE, fb->pixels);
    }

    // update output size
    tex_display_height = output_height;

    return buffer_size_changed;
}

void gl_screen_read(struct rdp_frame_buffer* fb, bool alpha)
{
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);

    fb->width = vp[2];
    fb->height = vp[3];
    fb->pitch = fb->width;

    if (fb->pixels) {
        glReadPixels(vp[0], vp[1], vp[2], vp[3], alpha ? GL_RGBA : GL_RGB, TEX_TYPE, fb->pixels);
    }
}

void gl_screen_render(int32_t win_width, int32_t win_height, int32_t win_x, int32_t win_y)
{
    int32_t hw = tex_display_height * win_width;
    int32_t wh = tex_width * win_height;

    // add letterboxes or pillarboxes if the window has a different aspect ratio
    // than the current display mode
    if (hw > wh) {
        int32_t w_max = wh / tex_display_height;
        win_x += (win_width - w_max) / 2;
        win_width = w_max;
    } else if (hw < wh) {
        int32_t h_max = hw / tex_width;
        win_y += (win_height - h_max) / 2;
        win_height = h_max;
    }

    // configure viewport
    glViewport(win_x, win_y, win_width, win_height);

    // draw fullscreen triangle
	glDepthMask(GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, 3);
	glDepthMask(GL_TRUE);

    // check if there was an error when using any of the commands above
    gl_check_errors();
}

void gl_screen_clear(void)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void gl_screen_close(void)
{
    tex_width = 0;
    tex_height = 0;

    tex_display_height = 0;

    glDeleteTextures(1, &texture);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
}
