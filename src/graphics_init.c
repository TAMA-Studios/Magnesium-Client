// A narrow GLFW hint hook for the pinned raylib Desktop backend.
// The UI does not enable depth testing or use stencil operations.
#undef glfwDefaultWindowHints
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

void MagnesiumDefaultWindowHints(void)
{
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);
}
