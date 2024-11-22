#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "mappings.h"
#include "config.h"
#include "options.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "input.h"
#include "ini.h"
#include "srm.h"
#include "utils.h"

extern GLFWwindow *window;
config g_cfg;

static void error_cb(int error, const char* description)
{
    fprintf(stderr, "Error: %s\n", description);
}

void joystick_callback(int jid, int event)
{
    if (event == GLFW_CONNECTED)
        printf("%s %s\n", glfwGetGamepadName(jid), glfwGetJoystickGUID(jid));
    else if (event == GLFW_DISCONNECTED)
        printf("Joypad %d disconnected\n", jid);
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (key == GLFW_KEY_ENTER && (mods & GLFW_MOD_ALT)) {
        if (glfwGetWindowMonitor(window) == NULL) {
            // Se estiver em modo janela, mude para tela cheia
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            // Se estiver em tela cheia, mude para modo janela
            glfwSetWindowMonitor(window, NULL, 100, 100, g_cfg.window_width, g_cfg.window_height, 0);
        }
    }
}

static void window_size_callback(GLFWwindow* window, int width, int height) {
    // Atualize a posição do cursor do mouse
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);
    glfwSetCursorPos(window, xpos * (double)width / g_cfg.window_width, ypos * (double)height / g_cfg.window_height);
    g_cfg.window_width = width;
    g_cfg.window_height = height;
}

int main(int argc, char *argv[]) {
    cfg_defaults(&g_cfg);
    if (ini_parse("./config.ini", cfg_handler, &g_cfg) < 0)
        die("Could not parse config.ini");

    ini_parse("./options.ini", opt_handler, NULL);

    glfwSetErrorCallback(error_cb);

    if (!glfwInit())
        die("Failed to initialize GLFW");

    if (!glfwUpdateGamepadMappings(mappings))
        die("Failed to load mappings");
    else
        printf("Updated mappings\n");

    glfwSetJoystickCallback(joystick_callback);

    core_load(g_cfg.core);
    core_load_game(g_cfg.rom);

    srm_load();

    glfwSwapInterval(g_cfg.swap_interval);

    // Registre a callback de teclado
    glfwSetKeyCallback(window, key_callback);

    // Registre a callback de redimensionamento da janela
    glfwSetWindowSizeCallback(window, window_size_callback);

    unsigned frame = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        input_poll();
        core_run();
        video_render();
        glfwSwapBuffers(window);
        frame++;
        if (frame % 600 == 0)
            srm_save();
    }

    srm_save();
    core_unload();
    audio_deinit();
    video_deinit();

    glfwTerminate();
    return 0;
}