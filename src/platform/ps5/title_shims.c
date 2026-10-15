#include "title_shims.h"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>

void __real_glDrawArrays(GLenum, GLint, GLsizei);
void __real_glDrawArraysInstanced(GLenum, GLint, GLsizei, GLsizei);
void __real_glDrawArraysInstancedBaseInstance(GLenum, GLint, GLsizei, GLsizei, GLuint);
void __real_glDrawElements(GLenum, GLsizei, GLenum, const void*);
void __real_glDrawElementsInstanced(GLenum, GLsizei, GLenum, const void*, GLsizei);
void __real_glBindFramebuffer(GLenum, GLuint);
void __real_glBlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
void __real_glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void __real_glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
void __real_glClear(GLbitfield);

void __wrap_glDrawArrays(GLenum a, GLint b, GLsizei c) { __real_glDrawArrays(a, b, c); }
void __wrap_glDrawArraysInstanced(GLenum a, GLint b, GLsizei c, GLsizei d) { __real_glDrawArraysInstanced(a, b, c, d); }
void __wrap_glDrawArraysInstancedBaseInstance(GLenum a, GLint b, GLsizei c, GLsizei d, GLuint e) { __real_glDrawArraysInstancedBaseInstance(a, b, c, d, e); }
void __wrap_glDrawElements(GLenum a, GLsizei b, GLenum c, const void* d) { __real_glDrawElements(a, b, c, d); }
void __wrap_glDrawElementsInstanced(GLenum a, GLsizei b, GLenum c, const void* d, GLsizei e) { __real_glDrawElementsInstanced(a, b, c, d, e); }
void __wrap_glBindFramebuffer(GLenum a, GLuint b) { __real_glBindFramebuffer(a, b); }
void __wrap_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield i, GLenum j) { __real_glBlitFramebuffer(a, b, c, d, e, f, g, h, i, j); }
void __wrap_glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void* i) { __real_glTexImage2D(a, b, c, d, e, f, g, h, i); }
void __wrap_glTexSubImage2D(GLenum a, GLint b, GLint c, GLint d, GLsizei e, GLsizei f, GLenum g, GLenum h, const void* i) { __real_glTexSubImage2D(a, b, c, d, e, f, g, h, i); }
void __wrap_glClear(GLbitfield a) { __real_glClear(a); }

int __real_sceSystemServiceHideSplashScreen(void);
static int splash_released;
void z3cloudplay_release_splash(void) { splash_released = 1; __real_sceSystemServiceHideSplashScreen(); }
int __wrap_sceSystemServiceHideSplashScreen(void) { return splash_released ? __real_sceSystemServiceHideSplashScreen() : 0; }
