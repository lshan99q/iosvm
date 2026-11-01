static void
remote_glAlphaFunc(void *context, GLenum a0, GLfloat a1)
{
	(void)context;
	gw_glAlphaFunc(a0, a1);
}

static void
remote_glBindTexture(void *context, GLenum a0, GLuint a1)
{
	(void)context;
	gw_glBindTexture(a0, a1);
}

static void
remote_glBlendFunc(void *context, GLenum a0, GLenum a1)
{
	(void)context;
	gw_glBlendFunc(a0, a1);
}

static void
remote_glBlendFuncSeparate(void *context, GLenum a0, GLenum a1, GLenum a2, GLenum a3)
{
	(void)context;
	gw_glBlendFuncSeparate(a0, a1, a2, a3);
}

static void
remote_glBlendEquation(void *context, GLenum a0)
{
	(void)context;
	gw_glBlendEquation(a0);
}

static void
remote_glClear(void *context, GLuint a0)
{
	(void)context;
	gw_glClear(a0);
}

static void
remote_glClearColor(void *context, GLfloat a0, GLfloat a1, GLfloat a2, GLfloat a3)
{
	(void)context;
	gw_glClearColor(a0, a1, a2, a3);
}

static void
remote_glClearStencil(void *context, GLint a0)
{
	(void)context;
	gw_glClearStencil(a0);
}

static void
remote_glCopyTexSubImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5, GLsizei a6, GLsizei a7)
{
	(void)context;
	gw_glCopyTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
}

static void
remote_glDeleteTextures(void *context, GLsizei a0, const void *a1, GLsizei n1)
{
	(void)context;
	gw_glDeleteTextures(a0, a1, n1);
}

static void
remote_glDisable(void *context, GLenum a0)
{
	(void)context;
	gw_glDisable(a0);
}

static void
remote_glDrawArrays(void *context, GLenum a0, GLint a1, GLsizei a2)
{
	(void)context;
	gw_glDrawArrays(a0, a1, a2);
}

static void
remote_glDrawElements(void *context, GLenum a0, GLsizei a1, GLenum a2, const void *a3, GLsizei n3)
{
	(void)context;
	gw_glDrawElements(a0, a1, a2, a3, n3);
}

static void
remote_glEnable(void *context, GLenum a0)
{
	(void)context;
	gw_glEnable(a0);
}

static void
remote_glFinish(void *context)
{
	(void)context;
	gw_glFinish();
}

static void
remote_glFlush(void *context)
{
	(void)context;
	gw_glFlush();
}

static void
remote_glGenTextures(void *context, GLsizei a0, const void *a1, GLsizei n1)
{
	(void)context;
	gw_glGenTextures(a0, a1, n1);
}

static void
remote_glGetIntegerv(void *context, GLenum a0, const void *a1, GLsizei n1)
{
	(void)context;
	gw_glGetIntegerv(a0, a1, n1);
}

static void
remote_glPixelStorei(void *context, GLenum a0, GLint a1)
{
	(void)context;
	gw_glPixelStorei(a0, a1);
}

static void
remote_glScissor(void *context, GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	(void)context;
	gw_glScissor(a0, a1, a2, a3);
}

static void
remote_glTexImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5, GLenum a6, GLenum a7, const void *a8, GLsizei n8)
{
	(void)context;
	gw_glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8, n8);
}

static void
remote_glTexParameterf(void *context, GLenum a0, GLenum a1, GLfloat a2)
{
	(void)context;
	gw_glTexParameterf(a0, a1, a2);
}

static void
remote_glTexParameteri(void *context, GLenum a0, GLenum a1, GLint a2)
{
	(void)context;
	gw_glTexParameteri(a0, a1, a2);
}

static void
remote_glTexSubImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5, GLenum a6, GLenum a7, const void *a8, GLsizei n8)
{
	(void)context;
	gw_glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8, n8);
}

static void
remote_glViewport(void *context, GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	(void)context;
	gw_glViewport(a0, a1, a2, a3);
}

static void
remote_glActiveTexture(void *context, GLenum a0)
{
	(void)context;
	gw_glActiveTexture(a0);
}

static void
remote_glCompressedTexImage2D(void *context, GLenum a0, GLint a1, GLenum a2, GLsizei a3, GLsizei a4, GLint a5, GLsizei a6, const void *a7, GLsizei n7)
{
	(void)context;
	gw_glCompressedTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, n7);
}

static void
remote_glVertexAttribPointer(void *context, GLuint a0, GLint a1, GLenum a2, GLboolean a3, GLsizei a4, const void *a5, GLsizei n5)
{
	(void)context;
	gw_glVertexAttribPointer(a0, a1, a2, a3, a4, a5, n5);
}

static void
remote_glEnableVertexAttribArray(void *context, GLuint a0)
{
	(void)context;
	gw_glEnableVertexAttribArray(a0);
}

static void
remote_glDisableVertexAttribArray(void *context, GLuint a0)
{
	(void)context;
	gw_glDisableVertexAttribArray(a0);
}

static void
remote_glDeleteProgram(void *context, GLuint a0)
{
	(void)context;
	gw_glDeleteProgram(a0);
}

static void
remote_glShaderSource(void *context, GLuint a0, GLsizei a1, const void *a2, GLsizei n2, const void *a3, GLsizei n3)
{
	(void)context;
	gw_glShaderSource(a0, a1, a2, n2, a3, n3);
}

static void
remote_glCompileShader(void *context, GLuint a0)
{
	(void)context;
	gw_glCompileShader(a0);
}

static void
remote_glAttachShader(void *context, GLuint a0, GLuint a1)
{
	(void)context;
	gw_glAttachShader(a0, a1);
}

static void
remote_glLinkProgram(void *context, GLuint a0)
{
	(void)context;
	gw_glLinkProgram(a0);
}

static void
remote_glUseProgram(void *context, GLuint a0)
{
	(void)context;
	gw_glUseProgram(a0);
}

static void
remote_glUniform1f(void *context, GLint a0, GLfloat a1)
{
	(void)context;
	gw_glUniform1f(a0, a1);
}

static void
remote_glUniform4f(void *context, GLint a0, GLfloat a1, GLfloat a2, GLfloat a3, GLfloat a4)
{
	(void)context;
	gw_glUniform4f(a0, a1, a2, a3, a4);
}

static void
remote_glUniform1i(void *context, GLint a0, GLint a1)
{
	(void)context;
	gw_glUniform1i(a0, a1);
}

static void
remote_glUniform3fv(void *context, GLint a0, GLsizei a1, const void *a2, GLsizei n2)
{
	(void)context;
	gw_glUniform3fv(a0, a1, a2, n2);
}

static void
remote_glUniform4fv(void *context, GLint a0, GLsizei a1, const void *a2, GLsizei n2)
{
	(void)context;
	gw_glUniform4fv(a0, a1, a2, n2);
}

static void
remote_glUniformMatrix3fv(void *context, GLint a0, GLsizei a1, GLboolean a2, const void *a3, GLsizei n3)
{
	(void)context;
	gw_glUniformMatrix3fv(a0, a1, a2, a3, n3);
}

static void
remote_glUniformMatrix4fv(void *context, GLint a0, GLsizei a1, GLboolean a2, const void *a3, GLsizei n3)
{
	(void)context;
	gw_glUniformMatrix4fv(a0, a1, a2, a3, n3);
}

static void
remote_glBindAttribLocation(void *context, GLuint a0, GLuint a1, const void *a2, GLsizei n2)
{
	(void)context;
	gw_glBindAttribLocation(a0, a1, a2, n2);
}

static void
remote_glBindFramebuffer(void *context, GLenum a0, GLuint a1)
{
	(void)context;
	gw_glBindFramebuffer(a0, a1);
}

static void
remote_glDeleteFramebuffers(void *context, GLsizei a0, const void *a1, GLsizei n1)
{
	(void)context;
	gw_glDeleteFramebuffers(a0, a1, n1);
}

static void
remote_glGenFramebuffers(void *context, GLsizei a0, const void *a1, GLsizei n1)
{
	(void)context;
	gw_glGenFramebuffers(a0, a1, n1);
}

static void
remote_glFramebufferTexture2D(void *context, GLenum a0, GLenum a1, GLenum a2, GLuint a3, GLint a4)
{
	(void)context;
	gw_glFramebufferTexture2D(a0, a1, a2, a3, a4);
}

static void
remote_glGenerateMipmap(void *context, GLenum a0)
{
	(void)context;
	gw_glGenerateMipmap(a0);
}

static void
remote_glDeleteSync(void *context, const void *a0, GLsizei n0)
{
	(void)context;
	gw_glDeleteSync(a0, n0);
}

