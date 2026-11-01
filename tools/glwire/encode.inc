void gw_glAlphaFunc(GLenum a0, GLfloat a1)
{
	wire_open(&w, 0);
	wire_u32(&w, a0);
	wire_f32(&w, a1);
	wire_close(&w);
}

void gw_glBindTexture(GLenum a0, GLuint a1)
{
	wire_open(&w, 1);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

void gw_glBlendFunc(GLenum a0, GLenum a1)
{
	wire_open(&w, 2);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

void gw_glBlendFuncSeparate(GLenum a0, GLenum a1, GLenum a2, GLenum a3)
{
	wire_open(&w, 3);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_u32(&w, a2);
	wire_u32(&w, a3);
	wire_close(&w);
}

void gw_glBlendEquation(GLenum a0)
{
	wire_open(&w, 4);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glClear(GLuint a0)
{
	wire_open(&w, 5);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glClearColor(GLfloat a0, GLfloat a1, GLfloat a2, GLfloat a3)
{
	wire_open(&w, 6);
	wire_f32(&w, a0);
	wire_f32(&w, a1);
	wire_f32(&w, a2);
	wire_f32(&w, a3);
	wire_close(&w);
}

void gw_glClearStencil(GLint a0)
{
	wire_open(&w, 7);
	wire_i32(&w, a0);
	wire_close(&w);
}

void gw_glCopyTexSubImage2D(GLenum a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5, GLsizei a6, GLsizei a7)
{
	wire_open(&w, 8);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_i32(&w, a3);
	wire_i32(&w, a4);
	wire_i32(&w, a5);
	wire_i32(&w, a6);
	wire_i32(&w, a7);
	wire_close(&w);
}

void gw_glDeleteTextures(GLsizei a0, const void *a1, GLsizei n1)
{
	wire_open(&w, 9);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, n1);
	wire_close(&w);
}

void gw_glDisable(GLenum a0)
{
	wire_open(&w, 10);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glDrawArrays(GLenum a0, GLint a1, GLsizei a2)
{
	wire_open(&w, 11);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_close(&w);
}

void gw_glDrawElements(GLenum a0, GLsizei a1, GLenum a2, const void *a3, GLsizei n3)
{
	wire_open(&w, 12);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_u32(&w, a2);
	wire_bytes(&w, a3, n3);
	wire_close(&w);
}

void gw_glEnable(GLenum a0)
{
	wire_open(&w, 13);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glFinish(void)
{
	wire_open(&w, 14);

	wire_close(&w);
}

void gw_glFlush(void)
{
	wire_open(&w, 15);

	wire_close(&w);
}

void gw_glGenTextures(GLsizei a0, const void *a1, GLsizei n1)
{
	wire_open(&w, 16);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, n1);
	wire_close(&w);
}

/* glGetError returns E: the caller reads it back after a flush */
void gw_glGetError(void)
{
	wire_open(&w, 17);

	wire_close(&w);
}

void gw_glGetIntegerv(GLenum a0, const void *a1, GLsizei n1)
{
	wire_open(&w, 18);
	wire_u32(&w, a0);
	wire_bytes(&w, a1, n1);
	wire_close(&w);
}

/* glGetString returns P: the caller reads it back after a flush */
void gw_glGetString(GLenum a0)
{
	wire_open(&w, 19);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glPixelStorei(GLenum a0, GLint a1)
{
	wire_open(&w, 20);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_close(&w);
}

void gw_glScissor(GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	wire_open(&w, 21);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_i32(&w, a3);
	wire_close(&w);
}

void gw_glTexImage2D(GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5, GLenum a6, GLenum a7, const void *a8, GLsizei n8)
{
	wire_open(&w, 22);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_i32(&w, a3);
	wire_i32(&w, a4);
	wire_i32(&w, a5);
	wire_u32(&w, a6);
	wire_u32(&w, a7);
	wire_bytes(&w, a8, n8);
	wire_close(&w);
}

void gw_glTexParameterf(GLenum a0, GLenum a1, GLfloat a2)
{
	wire_open(&w, 23);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_f32(&w, a2);
	wire_close(&w);
}

void gw_glTexParameteri(GLenum a0, GLenum a1, GLint a2)
{
	wire_open(&w, 24);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_i32(&w, a2);
	wire_close(&w);
}

void gw_glTexSubImage2D(GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5, GLenum a6, GLenum a7, const void *a8, GLsizei n8)
{
	wire_open(&w, 25);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_i32(&w, a3);
	wire_i32(&w, a4);
	wire_i32(&w, a5);
	wire_u32(&w, a6);
	wire_u32(&w, a7);
	wire_bytes(&w, a8, n8);
	wire_close(&w);
}

void gw_glViewport(GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	wire_open(&w, 26);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_i32(&w, a3);
	wire_close(&w);
}

void gw_glActiveTexture(GLenum a0)
{
	wire_open(&w, 27);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glCompressedTexImage2D(GLenum a0, GLint a1, GLenum a2, GLsizei a3, GLsizei a4, GLint a5, GLsizei a6, const void *a7, GLsizei n7)
{
	wire_open(&w, 28);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_u32(&w, a2);
	wire_i32(&w, a3);
	wire_i32(&w, a4);
	wire_i32(&w, a5);
	wire_i32(&w, a6);
	wire_bytes(&w, a7, n7);
	wire_close(&w);
}

void gw_glVertexAttribPointer(GLuint a0, GLint a1, GLenum a2, GLboolean a3, GLsizei a4, const void *a5, GLsizei n5)
{
	wire_open(&w, 29);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_u32(&w, a2);
	wire_u8(&w, a3);
	wire_i32(&w, a4);
	wire_bytes(&w, a5, n5);
	wire_close(&w);
}

void gw_glEnableVertexAttribArray(GLuint a0)
{
	wire_open(&w, 30);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glDisableVertexAttribArray(GLuint a0)
{
	wire_open(&w, 31);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glDeleteProgram(GLuint a0)
{
	wire_open(&w, 32);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glCreateShader returns U: the caller reads it back after a flush */
void gw_glCreateShader(GLenum a0)
{
	wire_open(&w, 33);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glShaderSource(GLuint a0, GLsizei a1, const void *a2, GLsizei n2, const void *a3, GLsizei n3)
{
	wire_open(&w, 34);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_bytes(&w, a2, n2);
	wire_bytes(&w, a3, n3);
	wire_close(&w);
}

void gw_glCompileShader(GLuint a0)
{
	wire_open(&w, 35);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glCreateProgram returns U: the caller reads it back after a flush */
void gw_glCreateProgram(void)
{
	wire_open(&w, 36);

	wire_close(&w);
}

void gw_glAttachShader(GLuint a0, GLuint a1)
{
	wire_open(&w, 37);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

void gw_glLinkProgram(GLuint a0)
{
	wire_open(&w, 38);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glUseProgram(GLuint a0)
{
	wire_open(&w, 39);
	wire_u32(&w, a0);
	wire_close(&w);
}

void gw_glUniform1f(GLint a0, GLfloat a1)
{
	wire_open(&w, 40);
	wire_i32(&w, a0);
	wire_f32(&w, a1);
	wire_close(&w);
}

void gw_glUniform4f(GLint a0, GLfloat a1, GLfloat a2, GLfloat a3, GLfloat a4)
{
	wire_open(&w, 41);
	wire_i32(&w, a0);
	wire_f32(&w, a1);
	wire_f32(&w, a2);
	wire_f32(&w, a3);
	wire_f32(&w, a4);
	wire_close(&w);
}

void gw_glUniform1i(GLint a0, GLint a1)
{
	wire_open(&w, 42);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_close(&w);
}

void gw_glUniform3fv(GLint a0, GLsizei a1, const void *a2, GLsizei n2)
{
	wire_open(&w, 43);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_bytes(&w, a2, n2);
	wire_close(&w);
}

void gw_glUniform4fv(GLint a0, GLsizei a1, const void *a2, GLsizei n2)
{
	wire_open(&w, 44);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_bytes(&w, a2, n2);
	wire_close(&w);
}

void gw_glUniformMatrix3fv(GLint a0, GLsizei a1, GLboolean a2, const void *a3, GLsizei n3)
{
	wire_open(&w, 45);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_u8(&w, a2);
	wire_bytes(&w, a3, n3);
	wire_close(&w);
}

void gw_glUniformMatrix4fv(GLint a0, GLsizei a1, GLboolean a2, const void *a3, GLsizei n3)
{
	wire_open(&w, 46);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_u8(&w, a2);
	wire_bytes(&w, a3, n3);
	wire_close(&w);
}

/* glGetUniformLocation returns I: the caller reads it back after a flush */
void gw_glGetUniformLocation(GLuint a0, const void *a1, GLsizei n1)
{
	wire_open(&w, 47);
	wire_u32(&w, a0);
	wire_bytes(&w, a1, n1);
	wire_close(&w);
}

void gw_glBindAttribLocation(GLuint a0, GLuint a1, const void *a2, GLsizei n2)
{
	wire_open(&w, 48);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_bytes(&w, a2, n2);
	wire_close(&w);
}

void gw_glBindFramebuffer(GLenum a0, GLuint a1)
{
	wire_open(&w, 49);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

void gw_glDeleteFramebuffers(GLsizei a0, const void *a1, GLsizei n1)
{
	wire_open(&w, 50);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, n1);
	wire_close(&w);
}

void gw_glGenFramebuffers(GLsizei a0, const void *a1, GLsizei n1)
{
	wire_open(&w, 51);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, n1);
	wire_close(&w);
}

void gw_glFramebufferTexture2D(GLenum a0, GLenum a1, GLenum a2, GLuint a3, GLint a4)
{
	wire_open(&w, 52);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_u32(&w, a2);
	wire_u32(&w, a3);
	wire_i32(&w, a4);
	wire_close(&w);
}

void gw_glGenerateMipmap(GLenum a0)
{
	wire_open(&w, 53);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glFenceSync returns P: the caller reads it back after a flush */
void gw_glFenceSync(GLenum a0, GLuint a1)
{
	wire_open(&w, 54);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

void gw_glDeleteSync(const void *a0, GLsizei n0)
{
	wire_open(&w, 55);
	wire_bytes(&w, a0, n0);
	wire_close(&w);
}

/* glClientWaitSync returns E: the caller reads it back after a flush */
void gw_glClientWaitSync(const void *a0, GLsizei n0, GLuint a1, GLuint a2)
{
	wire_open(&w, 56);
	wire_bytes(&w, a0, n0);
	wire_u32(&w, a1);
	wire_u32(&w, a2);
	wire_close(&w);
}

