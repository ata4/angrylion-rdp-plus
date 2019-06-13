#include "gl_screen.h"
#include "core/msg.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cuda_runtime.h>
#include <cuda_gl_interop.h>

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
static GLuint vbo;
struct cudaGraphicsResource *cuda_vbo_resource;
void *d_vbo_buffer = NULL;

static GLuint texture = 1;
static GLuint depth_texture = 2;

static GLint colorValueTextureLocation;
static GLint depthValueTextureLocation;

static int32_t tex_width;
static int32_t tex_height;

static float g_fAnim = 0.0;

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

static void gl_check_error_handle_cuda(void) {
	gl_check_errors();
	cudaDeviceReset();
	exit(EXIT_FAILURE);
}

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

		"uniform sampler2D ColorValueTexture;\n"
        "uniform sampler2D DepthValueTexture;\n"

        "void main(void) {\n"
#ifdef GLES
        "    color = texture(ColorValueTexture, uv);\n"
#else
        "    color.bgra = texture(ColorValueTexture, uv);\n"
#endif
		"    gl_FragDepth = texture(DepthValueTexture, uv).r;\n"
        "}\n";

    // compile and link OpenGL program
    GLuint vert = gl_shader_compile(GL_VERTEX_SHADER, vert_shader);
    GLuint frag = gl_shader_compile(GL_FRAGMENT_SHADER, frag_shader);
    program = gl_shader_link(vert, frag);

	// get the uniform variables location
	depthValueTextureLocation = glGetUniformLocation(program, "DepthValueTexture");
	colorValueTextureLocation = glGetUniformLocation(program, "ColorValueTexture");

	// specify the shader program to use
    glUseProgram(program);

	// bind the uniform variables locations
	glUniform1i(depthValueTextureLocation, 0);
	glUniform1i(colorValueTextureLocation, 1);

	// create VBO
	cudaMalloc((void **)&d_vbo_buffer, tex_width*tex_height * 4 * sizeof(float));

    // prepare dummy VAO
    glGenVertexArrays(1, &vbo);
    glBindVertexArray(vbo);

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

    // prepare depth texture
	glActiveTexture(GL_TEXTURE0 + 0);
    glGenTextures(1, &depth_texture);
    glBindTexture(GL_TEXTURE_2D, depth_texture);
	// configure interpolation method
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // prepare color texture
	glActiveTexture(GL_TEXTURE0 + 1);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
	// configure interpolation method
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

    // check if there was an error when using any of the commands above
    gl_check_errors();
}

void runCuda(struct cudaGraphicsResource **vbo_resource, struct rdp_frame_buffer* fb)
{
	// map OpenGL buffer object for writing from CUDA
	float4 *dptr;
	cudaGraphicsMapResources(1, vbo_resource, 0);
	size_t num_bytes;
	cudaGraphicsResourceGetMappedPointer((void **)&dptr, &num_bytes, *vbo_resource);
	//printf("CUDA mapped VBO: May access %ld bytes\n", num_bytes);

	// execute the kernel
	//    dim3 block(8, 8, 1);
	//    dim3 grid(mesh_width / block.x, mesh_height / block.y, 1);
	//    kernel<<< grid, block>>>(dptr, mesh_width, mesh_height, g_fAnim);

	//launch_kernel(dptr, fb->width, fb->height, g_fAnim);

	// unmap buffer object
	cudaGraphicsUnmapResources(1, vbo_resource, 0);
}

bool gl_screen_write(struct rdp_frame_buffer* fb, int32_t output_height)
{
    bool buffer_size_changed = tex_width != fb->width || tex_height != fb->height;
    
	// create buffer object
	glGenBuffers(1, vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);

	// initialize buffer object
	GLuint size = tex_width * tex_height * 4 * sizeof(float);
	glBufferData(GL_ARRAY_BUFFER, size, 0, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

    // check if the framebuffer size has changed
    if (buffer_size_changed) {
        tex_width = fb->width;
        tex_height = fb->height;

		// select the color value binding
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, texture);

		// set pitch for all unpacking operations
		glPixelStorei(GL_UNPACK_ROW_LENGTH, fb->pitch);
        // reallocate texture buffer on GPU
		glDepthMask(false);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_width, tex_height, 0, TEX_FORMAT, TEX_TYPE, fb->pixels);
		glDepthMask(true);

		// select the depth value binding
		glActiveTexture(GL_TEXTURE0 + 0);
		glBindTexture(GL_TEXTURE_2D, depth_texture);

		// set pitch for all unpacking operations
		glPixelStorei(GL_UNPACK_ROW_LENGTH, fb->pitch);
		// reallocate texture buffer on GPU
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glEnable(GL_DEPTH_TEST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_width, tex_height, 0, TEX_FORMAT, TEX_TYPE, fb->depth);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDisable(GL_DEPTH_TEST);


        msg_debug("%s: resized framebuffer texture: %dx%d", __FUNCTION__, tex_width, tex_height);
    } else {
		// select the color value binding
		glActiveTexture(GL_TEXTURE0 + 1);
		glBindTexture(GL_TEXTURE_2D, texture);

        // copy local buffer to GPU texture buffer
		glDepthMask(false);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_width, tex_height, TEX_FORMAT, TEX_TYPE, fb->pixels);
		glDepthMask(true);

		// select the depth value binding
		glActiveTexture(GL_TEXTURE0 + 0);
		glBindTexture(GL_TEXTURE_2D, depth_texture);

		// copy local buffer to GPU texture buffer
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glEnable(GL_DEPTH_TEST);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_width, tex_height, TEX_FORMAT, TEX_TYPE, fb->depth);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDisable(GL_DEPTH_TEST);

    }

	// register this buffer object with CUDA
	cudaGraphicsGLRegisterBuffer(&cuda_vbo_resource, vbo, -1);
	// run CUDA
	runCuda(&cuda_vbo_resource, fb);
	// check if there was an error when using any of the commands above
	gl_check_errors();

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


	// select the color value binding
	glActiveTexture(GL_TEXTURE0 + 1);
	glBindTexture(GL_TEXTURE_2D, texture);

	// draw
	glDrawArrays(GL_TRIANGLES, 0, 3);

	// select the depth value binding
	glActiveTexture(GL_TEXTURE0 + 0);
	glBindTexture(GL_TEXTURE_2D, depth_texture);

	// draw
	glEnable(GL_DEPTH_TEST);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisable(GL_DEPTH_TEST);


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
    glDeleteVertexArrays(1, &vbo);
    glDeleteProgram(program);
}
