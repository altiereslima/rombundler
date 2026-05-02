#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "libretro.h"
#include "config.h"
#include "utils.h"
#include "shaders.h"
#include "aspect.h"
#include "video_vulkan.h"
#include "core.h"
#include "font.h"

#ifdef __APPLE__
#define glGenVertexArrays glGenVertexArraysAPPLE
#define glBindVertexArray glBindVertexArrayAPPLE
#define glDeleteVertexArrays glDeleteVertexArraysAPPLE
#define glBindFramebuffer glBindFramebufferEXT
#define glGenFramebuffers glGenFramebuffersEXT
#define glGenRenderbuffers glGenRenderbuffersEXT
#define glBindRenderbuffer glBindRenderbufferEXT
#define glFramebufferTexture2D glFramebufferTexture2DEXT
#define glCheckFramebufferStatus glCheckFramebufferStatusEXT
#define glRenderbufferStorage glRenderbufferStorageEXT
#define glFramebufferRenderbuffer glFramebufferRenderbufferEXT
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_EXT
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#define GL_RENDERBUFFER GL_RENDERBUFFER_EXT
#define GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE_EXT
#define GL_DEPTH_ATTACHMENT GL_DEPTH_ATTACHMENT_EXT
#define GL_STENCIL_ATTACHMENT GL_STENCIL_ATTACHMENT_EXT
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_EXT
#endif

GLFWwindow *window = NULL;
extern config g_cfg;

static struct {
	GLuint tex_id;
	GLuint fbo_id;
	GLuint rbo_id;

	GLuint pitch;
	GLint tex_w, tex_h;
	GLuint clip_w, clip_h;
	float aspect_ratio;

	GLuint pixfmt;
	GLuint pixtype;
	GLuint bpp;

	struct retro_hw_render_callback hw;
} video = {0};

static struct {
	GLuint vao;
	GLuint vbo;
	GLuint program;

	GLint i_pos;
	GLint i_coord;
	GLint u_tex;
	GLint u_tex_size;
	GLint u_mvp;
} shader = {0};

static unsigned long long video_refresh_count = 0;
static unsigned long long video_probe_count = 0;
static unsigned long long video_proc_lookup_count = 0;
static unsigned long long video_fb_callback_count = 0;
static int video_hw_present_path = -1; /* -1 unknown, 0 frontend FBO, 1 default framebuffer */
static int video_last_frame_mode = 0; /* 0 unknown, 1 hw_fbo, 2 software */
static bool is_fullscreen = false;
static int saved_x = 0, saved_y = 0;
static int saved_w = 800, saved_h = 600;

void video_apply_cursor_mode(void)
{
	if (!window)
		return;

	glfwSetInputMode(window, GLFW_CURSOR,
		(is_fullscreen || g_cfg.hide_cursor) ? GLFW_CURSOR_HIDDEN : GLFW_CURSOR_NORMAL);
}

static GLuint compile_shader(unsigned type, unsigned count, const char **strings)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, count, strings, NULL);
	glCompileShader(shader);

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

	if (status == GL_FALSE) {
		char buffer[4096];
		glGetShaderInfoLog(shader, sizeof(buffer), NULL, buffer);
		die("Failed to compile %s shader: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", buffer);
	}

	return shader;
}

static bool core_needs_packed_depth_stencil(void)
{
	return g_cfg.core &&
		(strstr(g_cfg.core, "mupen64plus_next") != NULL ||
		 strstr(g_cfg.core, "mupen64plus") != NULL);
}

bool video_hw_context_supported(unsigned context_type)
{
	switch (context_type) {
		case RETRO_HW_CONTEXT_NONE:
		case RETRO_HW_CONTEXT_OPENGL:
		case RETRO_HW_CONTEXT_OPENGL_CORE:
		case RETRO_HW_CONTEXT_OPENGLES2:
			return true;
		case RETRO_HW_CONTEXT_VULKAN:
			return video_vulkan_supported();
		default:
			return false;
	}
}

bool video_uses_vulkan(void)
{
	return video.hw.context_type == RETRO_HW_CONTEXT_VULKAN;
}

bool video_menu_supported(void)
{
	return true;
}

void video_set_fast_forward(bool enabled)
{
	if (video_uses_vulkan())
		video_vulkan_set_fast_forward(enabled);
}

static float resolve_core_aspect_ratio(const struct retro_game_geometry *geom)
{
	if (geom && geom->aspect_ratio > 0.0f)
		return geom->aspect_ratio;

	if (geom && geom->base_width > 0 && geom->base_height > 0)
		return (float)geom->base_width / (float)geom->base_height;

	return 4.0f / 3.0f;
}

static void destroy_render_targets(void)
{
	video_hw_present_path = -1;
	video_last_frame_mode = 0;

	if (video.rbo_id) {
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glDeleteRenderbuffers(1, &video.rbo_id);
		video.rbo_id = 0;
	}

	if (video.fbo_id) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &video.fbo_id);
		video.fbo_id = 0;
	}

	if (video.tex_id) {
		glBindTexture(GL_TEXTURE_2D, 0);
		glDeleteTextures(1, &video.tex_id);
		video.tex_id = 0;
	}
}

static void reset_frontend_gl_state(void)
{
	glActiveTexture(GL_TEXTURE0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2) {
		glDrawBuffer(GL_BACK);
		glReadBuffer(GL_BACK);
	}
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);

	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);

#ifdef GL_FRAMEBUFFER_SRGB
	glDisable(GL_FRAMEBUFFER_SRGB);
#endif
}

static void probe_fbo_color(const char *stage)
{
	unsigned char pixel[4] = {0, 0, 0, 0};
	int probe_x = video.clip_w ? (int)(video.clip_w / 2) : 0;
	int probe_y = video.clip_h ? (int)(video.clip_h / 2) : 0;

	if (!video.fbo_id || !video.clip_w || !video.clip_h)
		return;

	video_probe_count++;
	if (video_probe_count > 10 && (video_probe_count % 300) != 0)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, video.fbo_id);
	if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2)
		glReadBuffer(GL_COLOR_ATTACHMENT0);
	glReadPixels(probe_x, probe_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	log_printf("video", "%s probe rgba=(%u,%u,%u,%u) at (%d,%d)",
		stage ? stage : "fbo",
		pixel[0], pixel[1], pixel[2], pixel[3],
		probe_x, probe_y);
}

static void probe_default_color(const char *stage)
{
	unsigned char pixel[4] = {0, 0, 0, 0};
	int fbw = 0, fbh = 0;
	int probe_x = 0;
	int probe_y = 0;

	if (!window)
		return;

	glfwGetFramebufferSize(window, &fbw, &fbh);
	if (!fbw || !fbh)
		return;

	if (video_probe_count > 10 && (video_probe_count % 300) != 0)
		return;

	probe_x = fbw / 2;
	probe_y = fbh / 2;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2)
		glReadBuffer(GL_BACK);
	glReadPixels(probe_x, probe_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	log_printf("video", "%s default probe rgba=(%u,%u,%u,%u) at (%d,%d)",
		stage ? stage : "default",
		pixel[0], pixel[1], pixel[2], pixel[3],
		probe_x, probe_y);
}

static bool core_may_self_present(void)
{
	return g_cfg.core &&
		(strstr(g_cfg.core, "mupen64plus_next") != NULL ||
		 strstr(g_cfg.core, "mupen64plus") != NULL);
}

static bool sample_default_backbuffer(int x, int y, unsigned char pixel[4])
{
	if (!pixel)
		return false;

	memset(pixel, 0, 4);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2)
		glReadBuffer(GL_BACK);
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	return true;
}

static void log_framebuffer_state(const char *stage)
{
	GLint framebuffer_binding = 0;
	GLint draw_buffer = 0;
	GLint read_buffer = 0;
	GLenum status = GL_FRAMEBUFFER_COMPLETE;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer_binding);
	if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2) {
		glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
		glGetIntegerv(GL_READ_BUFFER, &read_buffer);
	}
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	log_printf("video",
		"%s framebuffer=%d draw=0x%X read=0x%X status=0x%X",
		stage ? stage : "framebuffer_state",
		framebuffer_binding,
		draw_buffer,
		read_buffer,
		status);
}

static void log_gl_errors(const char *stage)
{
	GLenum err = GL_NO_ERROR;
	bool found = false;

	while ((err = glGetError()) != GL_NO_ERROR) {
		log_printf("gl", "%s -> GL error 0x%X", stage ? stage : "unknown", err);
		found = true;
	}

	if (found)
		fflush(stderr);
}

static void ortho2d(float m[4][4], float left, float right, float bottom, float top)
{
	m[0][0] = 1; m[0][1] = 0; m[0][2] = 0; m[0][3] = 0;
	m[1][0] = 0; m[1][1] = 1; m[1][2] = 0; m[1][3] = 0;
	m[2][0] = 0; m[2][1] = 0; m[2][2] = 1; m[2][3] = 0;
	m[3][0] = 0; m[3][1] = 0; m[3][2] = 0; m[3][3] = 1;

	m[0][0] = 2.0f / (right - left);
	m[1][1] = 2.0f / (top - bottom);
	m[2][2] = -1.0f;
	m[3][0] = -(right + left) / (right - left);
	m[3][1] = -(top + bottom) / (top - bottom);
}

static void core_ratio_viewport()
{
	int fbw = 0, fbh = 0;
	glfwGetFramebufferSize(window, &fbw, &fbh);

	aspect_viewport_t vp = aspect_calc(fbw, fbh, video.clip_w, video.clip_h, video.aspect_ratio);

	float x = vp.x;
	float y = fbh - (vp.y + vp.h); /* OpenGL origin is bottom-left, our Y is top-down */
	float w = vp.w;
	float h = vp.h;

	float ffbw = (float)fbw;
	float ffbh = (float)fbh;

	float x1 = x;
	float x2 = x;
	float x3 = x + w;
	float x4 = x + w;
	float y1 = y;
	float y2 = y + h;
	float y3 = y;
	float y4 = y + h;

	float vertex_data[] = {
		//  X, Y, U, V
		x1/ffbw*2 - 1, y1/ffbh*2 - 1, 0, 1, // left-bottom
		x2/ffbw*2 - 1, y2/ffbh*2 - 1, 0, 0, // left-top
		x3/ffbw*2 - 1, y3/ffbh*2 - 1, 1, 1, // right-bottom
		x4/ffbw*2 - 1, y4/ffbh*2 - 1, 1, 0, // right-top
	};

	glBindVertexArray(shader.vao);

	glBindBuffer(GL_ARRAY_BUFFER, shader.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);

	glEnableVertexAttribArray(shader.i_pos);
	glEnableVertexAttribArray(shader.i_coord);
	glVertexAttribPointer(shader.i_pos, 2, GL_FLOAT, GL_FALSE, sizeof(float)*4, 0);
	glVertexAttribPointer(shader.i_coord, 2, GL_FLOAT, GL_FALSE, sizeof(float)*4, (void*)(2 * sizeof(float)));

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void init_shaders()
{
	/* If recompiling, delete old program but keep VAO/VBO */
	if (shader.program) {
		glDeleteProgram(shader.program);
		shader.program = 0;
	}

	GLuint vshader = compile_shader(GL_VERTEX_SHADER, 1, &vshader_default_src);
	GLuint fshader = 0;
	if (strcmp(g_cfg.shader, "zfast-crt") == 0)
		fshader = compile_shader(GL_FRAGMENT_SHADER, 1, &fshader_zfastcrt_src);
	else if (strcmp(g_cfg.shader, "zfast-lcd") == 0)
		fshader = compile_shader(GL_FRAGMENT_SHADER, 1, &fshader_zfastlcd_src);
	else
		fshader = compile_shader(GL_FRAGMENT_SHADER, 1, &fshader_default_src);
	GLuint program = glCreateProgram();

	assert(program);

	glAttachShader(program, vshader);
	glAttachShader(program, fshader);
	glLinkProgram(program);

	glDeleteShader(vshader);
	glDeleteShader(fshader);

	glValidateProgram(program);

	GLint status;
	glGetProgramiv(program, GL_LINK_STATUS, &status);

	if (status == GL_FALSE) {
		char buffer[4096];
		glGetProgramInfoLog(program, sizeof(buffer), NULL, buffer);
		die("Failed to link shader program: %s", buffer);
	}

	shader.program    = program;
	shader.i_pos      = glGetAttribLocation(program,  "i_pos");
	shader.i_coord    = glGetAttribLocation(program,  "i_coord");
	shader.u_tex      = glGetUniformLocation(program, "u_tex");
	shader.u_tex_size = glGetUniformLocation(program, "u_tex_size");
	shader.u_mvp      = glGetUniformLocation(program, "u_mvp");

	/* Only create VAO/VBO on first init, reuse on recompile */
	if (!shader.vao) glGenVertexArrays(1, &shader.vao);
	if (!shader.vbo) glGenBuffers(1, &shader.vbo);

	glUseProgram(shader.program);
	glUniform1i(shader.u_tex, 0);

	float m[4][4];
	if (video.hw.bottom_left_origin)
		ortho2d(m, -1, 1, 1, -1);
	else
		ortho2d(m, -1, 1, -1, 1);

	glUniformMatrix4fv(shader.u_mvp, 1, GL_FALSE, (float*)m);
	glUseProgram(0);
}

void video_reload_shader(void)
{
	if (!window) return;
	init_shaders();
	/* Rebuild viewport geometry with current aspect */
	core_ratio_viewport();
}

void video_reload_filter(void)
{
	if (!video.tex_id) return;

	int filter = GL_NEAREST;
	if (strcmp(g_cfg.filter, "linear") == 0)
		filter = GL_LINEAR;

	glBindTexture(GL_TEXTURE_2D, video.tex_id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void create_window(int width, int height)
{
	unsigned context_major = video.hw.version_major;
	unsigned context_minor = video.hw.version_minor;
	bool profile_hint_supported = false;
	bool request_core_profile = false;
	bool use_vulkan = video_uses_vulkan();

	glfwDefaultWindowHints();

	if (video.hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE && context_major == 0) {
		context_major = 3;
		context_minor = 3;
	}

	profile_hint_supported =
		(context_major > 3) ||
		(context_major == 3 && context_minor >= 2);
	request_core_profile =
		(video.hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE) &&
		profile_hint_supported;

	log_printf("video", "create_window requested: %dx%d context=%u version=%u.%u fullscreen=%d",
		width, height, video.hw.context_type, context_major, context_minor, g_cfg.fullscreen);

	saved_w = g_cfg.window_width > 0 ? g_cfg.window_width : width;
	saved_h = g_cfg.window_height > 0 ? g_cfg.window_height : height;
	is_fullscreen = g_cfg.fullscreen;

	if (!use_vulkan && (video.hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE || context_major >= 3)) {
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, context_major);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, context_minor);
	}
	else if (!use_vulkan)
	{
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
	}

	bool use_core_profile = false;

	if (use_vulkan) {
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	} else {
		switch (video.hw.context_type) {
			case RETRO_HW_CONTEXT_OPENGL_CORE:
				/* GLFW only accepts profile hints on desktop GL 3.2+. */
				if (request_core_profile) {
					glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
					glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
					use_core_profile = true;
					log_printf("video", "requesting OpenGL Core Profile");
				} else {
					glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
					glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);
					log_printf("video", "requesting OpenGL %u.%u without profile hint", context_major, context_minor);
				}
				break;
			case RETRO_HW_CONTEXT_OPENGLES2:
				glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
				break;
			case RETRO_HW_CONTEXT_OPENGL:
			case RETRO_HW_CONTEXT_NONE:
				if (video.hw.version_major >= 3)
					glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
				break;
			default:
				die("Unsupported hw context %i.", video.hw.context_type);
		}
	}

	GLFWmonitor* monitor = NULL;
	if (g_cfg.fullscreen)
	{
		int count;
		monitor = glfwGetPrimaryMonitor();
		const GLFWvidmode *modes = glfwGetVideoModes(monitor, &count);
		const GLFWvidmode mode = modes[count-1];
		width = mode.width;
		height = mode.height;
	}

	window = glfwCreateWindow(width, height, g_cfg.title, monitor, NULL);

	/* Fallback: if Core profile failed, try Compat */
	if (!window && request_core_profile) {
		fprintf(stderr, "[video] Core profile failed, retrying with Compat Profile...\n");
		log_printf("video", "core profile creation failed, retrying with compat profile");
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
		glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);
		window = glfwCreateWindow(width, height, g_cfg.title, monitor, NULL);
		use_core_profile = false;
	}

	if (!window)
		die("Failed to create window.");

	if (!is_fullscreen) {
		glfwGetWindowPos(window, &saved_x, &saved_y);
		glfwGetWindowSize(window, &saved_w, &saved_h);
		g_cfg.window_width = saved_w;
		g_cfg.window_height = saved_h;
	}

	video_apply_cursor_mode();

	if (use_vulkan) {
		log_printf("video", "Vulkan window created");
		return;
	}

	glfwMakeContextCurrent(window);

	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
		die("Failed to initialize glad.");

	fprintf(stderr, "[video] GL context created: %s\n", (const char*)glGetString(GL_VERSION));
	log_printf("video", "GL vendor='%s' renderer='%s' version='%s'",
		(const char*)glGetString(GL_VENDOR),
		(const char*)glGetString(GL_RENDERER),
		(const char*)glGetString(GL_VERSION));

	init_shaders();
	glfwSwapInterval(g_cfg.swap_interval);

	/* GL_TEXTURE_2D enable is only valid in Compat profile */
	if (!use_core_profile && video.hw.context_type != RETRO_HW_CONTEXT_OPENGL_CORE)
		glEnable(GL_TEXTURE_2D);

	log_gl_errors("create_window");
}

void video_should_close(int v)
{
	if (!window)
		return;
	glfwSetWindowShouldClose(window, v);
}

static void init_framebuffer(int width, int height)
{
	bool use_packed_depth_stencil = core_needs_packed_depth_stencil() &&
		(video.hw.depth || video.hw.stencil);

	log_printf("video", "init_framebuffer %dx%d depth=%d stencil=%d packed_ds=%d",
		width, height, video.hw.depth, video.hw.stencil, use_packed_depth_stencil);
	glGenFramebuffers(1, &video.fbo_id);
	glBindFramebuffer(GL_FRAMEBUFFER, video.fbo_id);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, video.tex_id, 0);

	if (use_packed_depth_stencil) {
		glGenRenderbuffers(1, &video.rbo_id);
		glBindRenderbuffer(GL_RENDERBUFFER, video.rbo_id);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
		                         GL_RENDERBUFFER, video.rbo_id);
	} else if (video.hw.depth) {
		glGenRenderbuffers(1, &video.rbo_id);
		glBindRenderbuffer(GL_RENDERBUFFER, video.rbo_id);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                         GL_RENDERBUFFER, video.rbo_id);
	}

	if (video.hw.depth || video.hw.stencil)
		glBindRenderbuffer(GL_RENDERBUFFER, 0);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "[video] FBO incomplete (status 0x%X), retrying with basic format...\n", status);

		/* Retry: delete and recreate texture with universally compatible format */
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (video.rbo_id) { glDeleteRenderbuffers(1, &video.rbo_id); video.rbo_id = 0; }
		glDeleteFramebuffers(1, &video.fbo_id);

		/* Recreate texture as plain RGBA */
		glBindTexture(GL_TEXTURE_2D, video.tex_id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);

		glGenFramebuffers(1, &video.fbo_id);
		glBindFramebuffer(GL_FRAMEBUFFER, video.fbo_id);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, video.tex_id, 0);

		/* Retry depth/stencil */
		if (video.hw.depth || video.hw.stencil) {
			glGenRenderbuffers(1, &video.rbo_id);
			glBindRenderbuffer(GL_RENDERBUFFER, video.rbo_id);
			if (use_packed_depth_stencil || (video.hw.depth && video.hw.stencil)) {
				glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
				glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				                         GL_RENDERBUFFER, video.rbo_id);
			} else {
				glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
				glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				                         GL_RENDERBUFFER, video.rbo_id);
			}
			glBindRenderbuffer(GL_RENDERBUFFER, 0);
		}

		status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
			fprintf(stderr, "[video] FBO still incomplete (0x%X) — HW core may manage its own FBO\n", status);
		else
			fprintf(stderr, "[video] FBO retry succeeded\n");
	}

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	log_framebuffer_state("init_framebuffer");
	log_gl_errors("init_framebuffer");
}

uintptr_t video_get_current_framebuffer()
{
	if (video_uses_vulkan())
		return 0;

	video_fb_callback_count++;
	if (video_fb_callback_count <= 10 || video_fb_callback_count % 300 == 0) {
		log_printf("video", "get_current_framebuffer #%llu -> %u",
			video_fb_callback_count, video.fbo_id);
	}

	return video.fbo_id;
}

retro_proc_address_t video_get_proc_address(const char *sym)
{
	if (video_uses_vulkan()) {
		log_printf("video", "get_proc_address requested on Vulkan path sym='%s'",
			sym ? sym : "(null)");
		return NULL;
	}

	retro_proc_address_t proc = (retro_proc_address_t)glfwGetProcAddress(sym);
	bool important_sym = false;

	if (sym) {
		important_sym =
			strstr(sym, "Framebuffer") != NULL ||
			strstr(sym, "Renderbuffer") != NULL ||
			strstr(sym, "DrawBuffer") != NULL ||
			strstr(sym, "ReadBuffer") != NULL;
	}

	video_proc_lookup_count++;
	if (!proc || important_sym || video_proc_lookup_count <= 40 || video_proc_lookup_count % 200 == 0) {
		log_printf("video", "get_proc_address #%llu sym='%s' -> %p",
			video_proc_lookup_count,
			sym ? sym : "(null)",
			(void*)proc);
	}

	return proc;
}

const struct retro_hw_render_interface *video_get_hw_render_interface(void)
{
	if (!video_uses_vulkan())
		return NULL;

	return video_vulkan_get_hw_render_interface();
}

bool video_set_hw_render_context_negotiation_interface(
	const struct retro_hw_render_context_negotiation_interface *iface)
{
	if (!iface)
		return false;

	if (!video_vulkan_supported())
		return false;

	return video_vulkan_set_negotiation_interface(iface);
}

bool video_get_hw_render_context_negotiation_interface_support(
	struct retro_hw_render_context_negotiation_interface *iface)
{
	if (!iface)
		return false;

	if (!video_vulkan_supported())
		return false;

	return video_vulkan_get_negotiation_interface_support(iface);
}

void video_set_hw(struct retro_hw_render_callback hw)
{
	video.hw = hw;
	log_printf("video", "video_set_hw type=%u version=%u.%u cache_context=%d",
		hw.context_type, hw.version_major, hw.version_minor, hw.cache_context);
}

static void noop() {}

void video_configure(const struct retro_game_geometry *geom)
{
	/* Only set GL 2.1 defaults if the core didn't request
	   a specific HW context via SET_HW_RENDER */
	if (!video.hw.context_reset) {
		video.hw.version_major   = 2;
		video.hw.version_minor   = 1;
		video.hw.context_type    = RETRO_HW_CONTEXT_OPENGL;
		video.hw.context_reset   = noop;
		video.hw.context_destroy = noop;
	}

	if (!window)
		create_window(g_cfg.window_width, g_cfg.window_height);

	if (video_uses_vulkan()) {
		log_printf("video", "video_configure Vulkan base=%ux%u max=%ux%u aspect=%.4f",
			geom->base_width, geom->base_height, geom->max_width, geom->max_height, geom->aspect_ratio);
		if (!video_vulkan_init(window, geom, &video.hw))
			die("Failed to initialize Vulkan video backend.");
		video.hw.context_reset();
		return;
	}

	destroy_render_targets();
	log_printf("video", "video_configure base=%ux%u max=%ux%u aspect=%.4f",
		geom->base_width, geom->base_height, geom->max_width, geom->max_height, geom->aspect_ratio);

	/* Preserve the core's software-upload pixel format even if it also requested
	   SET_HW_RENDER earlier. Mupen can still send software frames (angrylion)
	   after advertising a HW context. */
	if (!video.pixfmt) {
		video.pixfmt  = GL_UNSIGNED_SHORT_5_5_5_1;
		video.pixtype = GL_BGRA;
		video.bpp     = 2;
	}

	log_printf("video", "render path=frontend_fbo core='%s' hw_context=%u",
		g_cfg.core, video.hw.context_type);

	glGenTextures(1, &video.tex_id);

	if (!video.tex_id)
		die("Failed to create the video texture");

	glBindTexture(GL_TEXTURE_2D, video.tex_id);

	int filter = GL_NEAREST;
	if (strcmp(g_cfg.filter, "linear") == 0)
		filter = GL_LINEAR;

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* Allocate the frontend texture/FBO with a neutral RGBA8 upload signature.
	   Software frames use video.pixtype/video.pixfmt later in video_refresh(). */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, geom->max_width, geom->max_height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	init_framebuffer(geom->max_width, geom->max_height);

	video.tex_w = geom->max_width;
	video.tex_h = geom->max_height;
	video.clip_w = geom->base_width;
	video.clip_h = geom->base_height;
	video.pitch = geom->base_width * video.bpp;
	video.aspect_ratio = resolve_core_aspect_ratio(geom);

	if (!video.clip_w)
		video.clip_w = video.tex_w;
	if (!video.clip_h)
		video.clip_h = video.tex_h;

	core_ratio_viewport();

	log_printf("video", "context_reset callback begin");
	log_gl_errors("pre_context_reset");
	video.hw.context_reset();
	log_printf("video", "context_reset callback end fbo=%u tex=%u", video.fbo_id, video.tex_id);
	log_framebuffer_state("post_context_reset");
	log_gl_errors("post_context_reset");
}

bool video_set_system_av_info(const struct retro_system_av_info *av)
{
	if (!av)
		return false;

	fprintf(stderr,
	        "[video] SET_SYSTEM_AV_INFO: %ux%u (max %ux%u)\n",
	        av->geometry.base_width,
	        av->geometry.base_height,
	        av->geometry.max_width,
	        av->geometry.max_height);

	video_configure(&av->geometry);
	return true;
}

void video_set_geometry(const struct retro_game_geometry *geom)
{
	if (video_uses_vulkan()) {
		video_vulkan_set_geometry(geom);
		return;
	}

	video.tex_w = geom->max_width;
	video.tex_h = geom->max_height;
	video.clip_w = geom->base_width;
	video.clip_h = geom->base_height;
	video.aspect_ratio = resolve_core_aspect_ratio(geom);

	if (!video.clip_w)
		video.clip_w = video.tex_w;
	if (!video.clip_h)
		video.clip_h = video.tex_h;

	printf("Set geom %dx%d\n", video.clip_w, video.clip_h);
}

bool video_set_pixel_format(unsigned format)
{
	if (video.tex_id)
		die("Tried to change pixel format after initialization.");

	switch (format) {
		case RETRO_PIXEL_FORMAT_0RGB1555:
			video.pixfmt = GL_UNSIGNED_SHORT_5_5_5_1;
			video.pixtype = GL_BGRA;
			video.bpp = sizeof(uint16_t);
			break;
		case RETRO_PIXEL_FORMAT_XRGB8888:
			video.pixfmt = GL_UNSIGNED_INT_8_8_8_8_REV;
			video.pixtype = GL_BGRA;
			video.bpp = sizeof(uint32_t);
			break;
		case RETRO_PIXEL_FORMAT_RGB565:
			video.pixfmt  = GL_UNSIGNED_SHORT_5_6_5;
			video.pixtype = GL_RGB;
			video.bpp = sizeof(uint16_t);
			break;
		default:
			die("Unknown pixel type %u", format);
	}

	return true;
}

void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (video_uses_vulkan()) {
		video_vulkan_refresh(data, width, height, pitch);
		return;
	}

	video_refresh_count++;
	video.clip_h = height;
	video.clip_w = width;
	video.pitch = pitch;

	if (!video.clip_w)
		video.clip_w = video.tex_w;
	if (!video.clip_h)
		video.clip_h = video.tex_h;

	/* For HW rendering cores, data is either NULL (dup frame)
	   or RETRO_HW_FRAME_BUFFER_VALID ((void*)-1) meaning
	   "I rendered into the FBO, display it".  In both cases
	   do NOT touch GL state — we're inside core_run(). */
	if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) {
		if (data == RETRO_HW_FRAME_BUFFER_VALID)
			video_last_frame_mode = 1;
		if (video_refresh_count <= 10 || video_refresh_count % 300 == 0) {
			log_printf("video", "video_refresh #%llu mode=%s size=%ux%u pitch=%zu",
				video_refresh_count,
				data == RETRO_HW_FRAME_BUFFER_VALID ? "hw_fbo" : "dupe_or_null",
				width, height, pitch);
		}
		return;
	}

	if (video_refresh_count <= 10 || video_refresh_count % 300 == 0) {
		log_printf("video", "video_refresh #%llu mode=software_upload size=%ux%u pitch=%zu",
			video_refresh_count, width, height, pitch);
	}

	video_last_frame_mode = 2;

	/* Software rendering: upload pixel data to texture */
	core_ratio_viewport();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, video.tex_id);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, video.pitch / video.bpp);

	glUseProgram(shader.program);
	glUniform2f(shader.u_tex_size, (float)width, (float)height);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, video.pixtype, video.pixfmt, data);

	glUseProgram(0);
	log_gl_errors("video_refresh");
}

void video_gl_unbind(void)
{
	if (video_uses_vulkan()) {
		video_vulkan_begin_frame();
		return;
	}

	/* Clear all GL state before handing control to the core.
	   HW-rendering cores (N64/GlideN64, PPSSPP, Beetle PSX HW)
	   expect a clean slate so their own VAO/VBO/program binds work. */
	reset_frontend_gl_state();

	if (video.fbo_id) {
		/* Bind the core's FBO so it renders into our texture */
		glBindFramebuffer(GL_FRAMEBUFFER, video.fbo_id);
		if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2) {
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
		}
		glViewport(0, 0, video.tex_w, video.tex_h);
		if (video_refresh_count <= 10 || video_refresh_count % 300 == 0)
			log_framebuffer_state("video_gl_unbind");
	}

	log_gl_errors("video_gl_unbind");
}

void video_gl_rebind(void)
{
	if (video_uses_vulkan())
		return;

	/* Restore frontend state after core_run() finishes.
	   Unbind everything the core may have left bound. */
	probe_fbo_color("post_core_run");
	probe_default_color("post_core_run");
	reset_frontend_gl_state();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (video_refresh_count <= 10 || video_refresh_count % 300 == 0)
		log_framebuffer_state("video_gl_rebind");
	log_gl_errors("video_gl_rebind");
}

void video_render()
{
	if (video_uses_vulkan()) {
		video_vulkan_render();
		return;
	}

	int fbw = 0, fbh = 0;
	glfwGetFramebufferSize(window, &fbw, &fbh);

	bool has_hw_context = (video.hw.context_reset && video.hw.context_reset != noop && video.fbo_id);
	bool is_hw_core = has_hw_context && video_last_frame_mode == 1;
	bool use_frontend_fbo_present =
		has_hw_context &&
		(video_last_frame_mode == 1 ||
		(core_may_self_present() && video_last_frame_mode == 2));
	aspect_viewport_t vp = aspect_calc(fbw, fbh, video.clip_w, video.clip_h, video.aspect_ratio);
	int dst_x0 = vp.x;
	int dst_y0 = fbh - (vp.y + vp.h);
	int dst_x1 = vp.x + vp.w;
	int dst_y1 = fbh - vp.y;

	/* Always render to screen, not to FBO */
	reset_frontend_gl_state();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, fbw, fbh);
	if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2) {
		glDrawBuffer(GL_BACK);
		glReadBuffer(GL_BACK);
	}

	if (is_hw_core && core_may_self_present() && video_hw_present_path != 0) {
		unsigned char pixel[4] = {0, 0, 0, 0};
		int probe_x = (dst_x0 + dst_x1) / 2;
		int probe_y = (dst_y0 + dst_y1) / 2;

		if (sample_default_backbuffer(probe_x, probe_y, pixel) &&
			(pixel[0] || pixel[1] || pixel[2] || pixel[3])) {
			if (video_hw_present_path != 1) {
				log_printf("video",
					"detected self-presented default framebuffer rgba=(%u,%u,%u,%u) at (%d,%d)",
					pixel[0], pixel[1], pixel[2], pixel[3], probe_x, probe_y);
			}
			video_hw_present_path = 1;
			log_gl_errors("video_render_default_passthrough");
			return;
		}
	}

	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);

	if (use_frontend_fbo_present) {
		GLenum filter = strcmp(g_cfg.filter, "linear") == 0 ? GL_LINEAR : GL_NEAREST;

		if (video_hw_present_path != 0) {
			log_printf("video", "using frontend FBO present path");
			video_hw_present_path = 0;
		}

		if (is_hw_core) {
			probe_fbo_color("video_render_pre_blit");
			probe_default_color("video_render_pre_blit");
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, video.fbo_id);
		if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2)
			glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		if (video.hw.context_type != RETRO_HW_CONTEXT_OPENGLES2)
			glDrawBuffer(GL_BACK);
		glBlitFramebuffer(0, 0, video.clip_w, video.clip_h,
			dst_x0, dst_y0, dst_x1, dst_y1,
			GL_COLOR_BUFFER_BIT, filter);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		if (is_hw_core)
			probe_default_color("video_render_post_blit");

		log_gl_errors("video_render_hw_blit");
		return;
	}

	core_ratio_viewport();

	glUseProgram(shader.program);
	glUniform2f(shader.u_tex_size, (float)video.clip_w, (float)video.clip_h);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, video.tex_id);

	glBindVertexArray(shader.vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	glUseProgram(0);
	log_gl_errors("video_render");

	/* OSD message from core (e.g. PUAE virtual keyboard status) */
	{
		const char *osd = core_message_text();
		if (osd && osd[0]) {
			int fbw = 0, fbh = 0;
			glfwGetFramebufferSize(window, &fbw, &fbh);
			float osd_scale = (float)fbh / 480.0f * 1.2f;
			if (osd_scale < 0.8f) osd_scale = 0.8f;
			float x = 10.0f;
			float y = (float)fbh - font_text_height(osd_scale) - 8.0f;
			font_render_text(x, y, osd, FONT_COLOR_YELLOW, osd_scale, fbw, fbh);
		}
	}
}

bool video_is_fullscreen(void)
{
	return is_fullscreen;
}

void video_toggle_fullscreen(void)
{
	if (!window) return;

	if (!is_fullscreen) {
		/* Salva posição e tamanho da janela */
		glfwGetWindowPos(window, &saved_x, &saved_y);
		glfwGetWindowSize(window, &saved_w, &saved_h);
		g_cfg.window_width = saved_w;
		g_cfg.window_height = saved_h;

		/* Vai para tela cheia */
		GLFWmonitor *monitor = glfwGetPrimaryMonitor();
		const GLFWvidmode *mode = glfwGetVideoMode(monitor);
		glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
		is_fullscreen = true;
		g_cfg.fullscreen = true;
	} else {
		/* Volta para modo janela */
		glfwSetWindowMonitor(window, NULL, saved_x, saved_y, saved_w, saved_h, 0);
		is_fullscreen = false;
		g_cfg.fullscreen = false;
		g_cfg.window_width = saved_w;
		g_cfg.window_height = saved_h;
	}

	video_apply_cursor_mode();
	if (video_uses_vulkan())
		video_vulkan_mark_swapchain_dirty();
	else
		glfwSwapInterval(g_cfg.swap_interval);
}

void video_deinit()
{
	if (video.hw.context_destroy)
		video.hw.context_destroy();

	if (video_uses_vulkan()) {
		video_vulkan_deinit();
		if (window) {
			glfwDestroyWindow(window);
			window = NULL;
		}
		return;
	}

	destroy_render_targets();

	if (shader.vao)
		glDeleteVertexArrays(1, &shader.vao);

	if (shader.vbo)
		glDeleteBuffers(1, &shader.vbo);

	if (shader.program)
		glDeleteProgram(shader.program);

	if (window) {
		glfwDestroyWindow(window);
		window = NULL;
	}
}
