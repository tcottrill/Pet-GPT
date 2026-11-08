// Forwarding header: the bundled GL/eglew.h includes <GL/glew.h>, but the bundled
// GLEW header lives one level up as thirdparty/glew.h. Without this file that
// include silently found the host's own GLEW (a different version) where one was
// installed, and failed outright where none is (the Flatpak SDK).
#include "../glew.h"
