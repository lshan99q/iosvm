static void
remote_glAlphaFunc(void *context, GLenum a0, GLfloat a1)
{
	if (!remote_only && local_entry[0] != NULL) {
		((void (*)(void *, GLenum, GLfloat))local_entry[0])(context, a0, a1);
	}
	gw_glAlphaFunc(a0, a1);
}

static void
remote_glBindTexture(void *context, GLenum a0, GLuint a1)
{
	if (!remote_only && local_entry[1] != NULL) {
		((void (*)(void *, GLenum, GLuint))local_entry[1])(context, a0, a1);
	}
	gw_glBindTexture(a0, a1);
}

static void
remote_glBlendFunc(void *context, GLenum a0, GLenum a1)
{
	if (!remote_only && local_entry[2] != NULL) {
		((void (*)(void *, GLenum, GLenum))local_entry[2])(context, a0, a1);
	}
	gw_glBlendFunc(a0, a1);
}

static void
remote_glBlendFuncSeparate(void *context, GLenum a0, GLenum a1, GLenum a2, GLenum a3)
{
	if (!remote_only && local_entry[3] != NULL) {
		((void (*)(void *, GLenum, GLenum, GLenum, GLenum))local_entry[3])(context, a0, a1, a2, a3);
	}
	gw_glBlendFuncSeparate(a0, a1, a2, a3);
}

static void
remote_glBlendEquation(void *context, GLenum a0)
{
	if (!remote_only && local_entry[4] != NULL) {
		((void (*)(void *, GLenum))local_entry[4])(context, a0);
	}
	gw_glBlendEquation(a0);
}

static void
remote_glClear(void *context, GLuint a0)
{
	if (!remote_only && local_entry[5] != NULL) {
		((void (*)(void *, GLuint))local_entry[5])(context, a0);
	}
	gw_glClear(a0);
}

static void
remote_glClearColor(void *context, GLfloat a0, GLfloat a1, GLfloat a2, GLfloat a3)
{
	if (!remote_only && local_entry[6] != NULL) {
		((void (*)(void *, GLfloat, GLfloat, GLfloat, GLfloat))local_entry[6])(context, a0, a1, a2, a3);
	}
	gw_glClearColor(a0, a1, a2, a3);
}

static void
remote_glClearStencil(void *context, GLint a0)
{
	if (!remote_only && local_entry[7] != NULL) {
		((void (*)(void *, GLint))local_entry[7])(context, a0);
	}
	gw_glClearStencil(a0);
}

static void
remote_glCopyTexSubImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5, GLsizei a6, GLsizei a7)
{
	if (!remote_only && local_entry[8] != NULL) {
		((void (*)(void *, GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))local_entry[8])(context, a0, a1, a2, a3, a4, a5, a6, a7);
	}
	gw_glCopyTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
}

static void
remote_glDeleteTextures(void *context, GLsizei a0, const GLuint * a1)
{
	if (local_entry[9] != NULL) {
		((void (*)(void *, GLsizei, const GLuint *))local_entry[9])(context, a0, a1);
	}
	gw_glDeleteTextures(a0, a1);
}

static void
remote_glDisable(void *context, GLenum a0)
{
	if (!remote_only && local_entry[10] != NULL) {
		((void (*)(void *, GLenum))local_entry[10])(context, a0);
	}
	gw_glDisable(a0);
}

static void
remote_glDrawArrays(void *context, GLenum a0, GLint a1, GLsizei a2)
{
	if (!remote_only && local_entry[11] != NULL) {
		((void (*)(void *, GLenum, GLint, GLsizei))local_entry[11])(context, a0, a1, a2);
	}
	remote_emit_vertex_data(a1, a2, NULL, 0);
	gw_glDrawArrays(a0, a1, a2);
}

static void
remote_glDrawElements(void *context, GLenum a0, GLsizei a1, GLenum a2, const void * a3)
{
	if (!remote_only && local_entry[12] != NULL) {
		((void (*)(void *, GLenum, GLsizei, GLenum, const void *))local_entry[12])(context, a0, a1, a2, a3);
	}
	remote_emit_vertex_data(0, a1, a3, a2);
	gw_glDrawElements(a0, a1, a2, a3);
}

static void
remote_glEnable(void *context, GLenum a0)
{
	if (!remote_only && local_entry[13] != NULL) {
		((void (*)(void *, GLenum))local_entry[13])(context, a0);
	}
	gw_glEnable(a0);
}

static void
remote_glFinish(void *context)
{
	if (local_entry[14] != NULL) {
		((void (*)(void *))local_entry[14])(context);
	}
	gw_glFinish();
}

static void
remote_glFlush(void *context)
{
	if (local_entry[15] != NULL) {
		((void (*)(void *))local_entry[15])(context);
	}
	gw_glFlush();
}

static void
remote_glGenTextures(void *context, GLsizei a0, const GLuint * a1)
{
	if (local_entry[16] != NULL) {
		((void (*)(void *, GLsizei, const GLuint *))local_entry[16])(context, a0, a1);
	}
	gw_glGenTextures(a0, a1);
}

static GLenum
remote_glGetError(void *context)
{
	GLenum result = (GLenum)0;
	if (local_entry[17] != NULL) {
		result = ((GLenum (*)(void *))local_entry[17])(context);
	}
	gw_glGetError(result);
	return result;
}

static void
remote_glGetIntegerv(void *context, GLenum a0, GLint * a1)
{
	if (local_entry[18] != NULL) {
		((void (*)(void *, GLenum, GLint *))local_entry[18])(context, a0, a1);
	}
	gw_glGetIntegerv(a0, a1);
}

static void *
remote_glGetString(void *context, GLenum a0)
{
	void * result = (void *)0;
	if (local_entry[19] != NULL) {
		result = ((void * (*)(void *, GLenum))local_entry[19])(context, a0);
	}
	gw_glGetString(a0, result);
	return result;
}

static void
remote_glPixelStorei(void *context, GLenum a0, GLint a1)
{
	if (!remote_only && local_entry[20] != NULL) {
		((void (*)(void *, GLenum, GLint))local_entry[20])(context, a0, a1);
	}
	remote_note_pixel_store(a0, a1);
	gw_glPixelStorei(a0, a1);
}

static void
remote_glScissor(void *context, GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	if (!remote_only && local_entry[21] != NULL) {
		((void (*)(void *, GLint, GLint, GLsizei, GLsizei))local_entry[21])(context, a0, a1, a2, a3);
	}
	gw_glScissor(a0, a1, a2, a3);
}

static void
remote_glTexImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5, GLenum a6, GLenum a7, const void * a8)
{
	if (!remote_only && local_entry[22] != NULL) {
		((void (*)(void *, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))local_entry[22])(context, a0, a1, a2, a3, a4, a5, a6, a7, a8);
	}
	gw_glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}

static void
remote_glTexParameterf(void *context, GLenum a0, GLenum a1, GLfloat a2)
{
	if (!remote_only && local_entry[23] != NULL) {
		((void (*)(void *, GLenum, GLenum, GLfloat))local_entry[23])(context, a0, a1, a2);
	}
	gw_glTexParameterf(a0, a1, a2);
}

static void
remote_glTexParameteri(void *context, GLenum a0, GLenum a1, GLint a2)
{
	if (!remote_only && local_entry[24] != NULL) {
		((void (*)(void *, GLenum, GLenum, GLint))local_entry[24])(context, a0, a1, a2);
	}
	gw_glTexParameteri(a0, a1, a2);
}

static void
remote_glTexSubImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5, GLenum a6, GLenum a7, const void * a8)
{
	if (!remote_only && local_entry[25] != NULL) {
		((void (*)(void *, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))local_entry[25])(context, a0, a1, a2, a3, a4, a5, a6, a7, a8);
	}
	gw_glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}

static void
remote_glViewport(void *context, GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	if (!remote_only && local_entry[26] != NULL) {
		((void (*)(void *, GLint, GLint, GLsizei, GLsizei))local_entry[26])(context, a0, a1, a2, a3);
	}
	gw_glViewport(a0, a1, a2, a3);
}

static void
remote_glActiveTexture(void *context, GLenum a0)
{
	if (!remote_only && local_entry[27] != NULL) {
		((void (*)(void *, GLenum))local_entry[27])(context, a0);
	}
	gw_glActiveTexture(a0);
}

static void
remote_glCompressedTexImage2D(void *context, GLenum a0, GLint a1, GLenum a2, GLsizei a3, GLsizei a4, GLint a5, GLsizei a6, const void * a7)
{
	if (!remote_only && local_entry[28] != NULL) {
		((void (*)(void *, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *))local_entry[28])(context, a0, a1, a2, a3, a4, a5, a6, a7);
	}
	gw_glCompressedTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
}

static void
remote_glVertexAttribPointer(void *context, GLuint a0, GLint a1, GLenum a2, GLboolean a3, GLsizei a4, const void * a5)
{
	if (!remote_only && local_entry[29] != NULL) {
		((void (*)(void *, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *))local_entry[29])(context, a0, a1, a2, a3, a4, a5);
	}
	remote_note_vertex_pointer(a0, a1, a2, a3, a4, a5);
	gw_glVertexAttribPointer(a0, a1, a2, a3, a4, a5);
}

static void
remote_glEnableVertexAttribArray(void *context, GLuint a0)
{
	if (!remote_only && local_entry[30] != NULL) {
		((void (*)(void *, GLuint))local_entry[30])(context, a0);
	}
	remote_note_attrib_enabled(a0, 1);
	gw_glEnableVertexAttribArray(a0);
}

static void
remote_glDisableVertexAttribArray(void *context, GLuint a0)
{
	if (!remote_only && local_entry[31] != NULL) {
		((void (*)(void *, GLuint))local_entry[31])(context, a0);
	}
	remote_note_attrib_enabled(a0, 0);
	gw_glDisableVertexAttribArray(a0);
}

static void
remote_glDeleteProgram(void *context, GLuint a0)
{
	if (local_entry[32] != NULL) {
		((void (*)(void *, GLuint))local_entry[32])(context, a0);
	}
	gw_glDeleteProgram(a0);
}

static GLuint
remote_glCreateShader(void *context, GLenum a0)
{
	GLuint result = (GLuint)0;
	if (local_entry[33] != NULL) {
		result = ((GLuint (*)(void *, GLenum))local_entry[33])(context, a0);
	}
	gw_glCreateShader(a0, result);
	return result;
}

static void
remote_glShaderSource(void *context, GLuint a0, GLsizei a1, const GLchar *const * a2, const GLint * a3)
{
	if (local_entry[34] != NULL) {
		((void (*)(void *, GLuint, GLsizei, const GLchar *const *, const GLint *))local_entry[34])(context, a0, a1, a2, a3);
	}
	gw_glShaderSource(a0, a1, a2, a3);
}

static void
remote_glCompileShader(void *context, GLuint a0)
{
	if (local_entry[35] != NULL) {
		((void (*)(void *, GLuint))local_entry[35])(context, a0);
	}
	gw_glCompileShader(a0);
}

static GLuint
remote_glCreateProgram(void *context)
{
	GLuint result = (GLuint)0;
	if (local_entry[36] != NULL) {
		result = ((GLuint (*)(void *))local_entry[36])(context);
	}
	gw_glCreateProgram(result);
	return result;
}

static void
remote_glAttachShader(void *context, GLuint a0, GLuint a1)
{
	if (local_entry[37] != NULL) {
		((void (*)(void *, GLuint, GLuint))local_entry[37])(context, a0, a1);
	}
	gw_glAttachShader(a0, a1);
}

static void
remote_glLinkProgram(void *context, GLuint a0)
{
	if (local_entry[38] != NULL) {
		((void (*)(void *, GLuint))local_entry[38])(context, a0);
	}
	gw_glLinkProgram(a0);
}

static void
remote_glUseProgram(void *context, GLuint a0)
{
	if (!remote_only && local_entry[39] != NULL) {
		((void (*)(void *, GLuint))local_entry[39])(context, a0);
	}
	gw_glUseProgram(a0);
}

static void
remote_glUniform1f(void *context, GLint a0, GLfloat a1)
{
	if (!remote_only && local_entry[40] != NULL) {
		((void (*)(void *, GLint, GLfloat))local_entry[40])(context, a0, a1);
	}
	gw_glUniform1f(a0, a1);
}

static void
remote_glUniform4f(void *context, GLint a0, GLfloat a1, GLfloat a2, GLfloat a3, GLfloat a4)
{
	if (!remote_only && local_entry[41] != NULL) {
		((void (*)(void *, GLint, GLfloat, GLfloat, GLfloat, GLfloat))local_entry[41])(context, a0, a1, a2, a3, a4);
	}
	gw_glUniform4f(a0, a1, a2, a3, a4);
}

static void
remote_glUniform1i(void *context, GLint a0, GLint a1)
{
	if (!remote_only && local_entry[42] != NULL) {
		((void (*)(void *, GLint, GLint))local_entry[42])(context, a0, a1);
	}
	gw_glUniform1i(a0, a1);
}

static void
remote_glUniform3fv(void *context, GLint a0, GLsizei a1, const GLfloat * a2)
{
	if (!remote_only && local_entry[43] != NULL) {
		((void (*)(void *, GLint, GLsizei, const GLfloat *))local_entry[43])(context, a0, a1, a2);
	}
	gw_glUniform3fv(a0, a1, a2);
}

static void
remote_glUniform4fv(void *context, GLint a0, GLsizei a1, const GLfloat * a2)
{
	if (!remote_only && local_entry[44] != NULL) {
		((void (*)(void *, GLint, GLsizei, const GLfloat *))local_entry[44])(context, a0, a1, a2);
	}
	gw_glUniform4fv(a0, a1, a2);
}

static void
remote_glUniformMatrix3fv(void *context, GLint a0, GLsizei a1, GLboolean a2, const GLfloat * a3)
{
	if (!remote_only && local_entry[45] != NULL) {
		((void (*)(void *, GLint, GLsizei, GLboolean, const GLfloat *))local_entry[45])(context, a0, a1, a2, a3);
	}
	gw_glUniformMatrix3fv(a0, a1, a2, a3);
}

static void
remote_glUniformMatrix4fv(void *context, GLint a0, GLsizei a1, GLboolean a2, const GLfloat * a3)
{
	if (!remote_only && local_entry[46] != NULL) {
		((void (*)(void *, GLint, GLsizei, GLboolean, const GLfloat *))local_entry[46])(context, a0, a1, a2, a3);
	}
	gw_glUniformMatrix4fv(a0, a1, a2, a3);
}

static GLint
remote_glGetUniformLocation(void *context, GLuint a0, const GLchar * a1)
{
	GLint result = (GLint)0;
	if (local_entry[47] != NULL) {
		result = ((GLint (*)(void *, GLuint, const GLchar *))local_entry[47])(context, a0, a1);
	}
	gw_glGetUniformLocation(a0, a1, result);
	return result;
}

static void
remote_glBindAttribLocation(void *context, GLuint a0, GLuint a1, const GLchar * a2)
{
	if (local_entry[48] != NULL) {
		((void (*)(void *, GLuint, GLuint, const GLchar *))local_entry[48])(context, a0, a1, a2);
	}
	gw_glBindAttribLocation(a0, a1, a2);
}

static void
remote_glBindFramebuffer(void *context, GLenum a0, GLuint a1)
{
	if (!remote_only && local_entry[49] != NULL) {
		((void (*)(void *, GLenum, GLuint))local_entry[49])(context, a0, a1);
	}
	gw_glBindFramebuffer(a0, a1);
}

static void
remote_glDeleteFramebuffers(void *context, GLsizei a0, const GLuint * a1)
{
	if (local_entry[50] != NULL) {
		((void (*)(void *, GLsizei, const GLuint *))local_entry[50])(context, a0, a1);
	}
	gw_glDeleteFramebuffers(a0, a1);
}

static void
remote_glGenFramebuffers(void *context, GLsizei a0, const GLuint * a1)
{
	if (local_entry[51] != NULL) {
		((void (*)(void *, GLsizei, const GLuint *))local_entry[51])(context, a0, a1);
	}
	gw_glGenFramebuffers(a0, a1);
}

static void
remote_glFramebufferTexture2D(void *context, GLenum a0, GLenum a1, GLenum a2, GLuint a3, GLint a4)
{
	if (!remote_only && local_entry[52] != NULL) {
		((void (*)(void *, GLenum, GLenum, GLenum, GLuint, GLint))local_entry[52])(context, a0, a1, a2, a3, a4);
	}
	gw_glFramebufferTexture2D(a0, a1, a2, a3, a4);
}

static void
remote_glGenerateMipmap(void *context, GLenum a0)
{
	if (!remote_only && local_entry[53] != NULL) {
		((void (*)(void *, GLenum))local_entry[53])(context, a0);
	}
	gw_glGenerateMipmap(a0);
}

static void *
remote_glFenceSync(void *context, GLenum a0, GLuint a1)
{
	void * result = (void *)0;
	if (local_entry[54] != NULL) {
		result = ((void * (*)(void *, GLenum, GLuint))local_entry[54])(context, a0, a1);
	}
	gw_glFenceSync(a0, a1, result);
	return result;
}

static void
remote_glDeleteSync(void *context, void * a0)
{
	if (local_entry[55] != NULL) {
		((void (*)(void *, void *))local_entry[55])(context, a0);
	}
	gw_glDeleteSync(a0);
}

static GLenum
remote_glClientWaitSync(void *context, void * a0, GLuint a1, GLuint a2)
{
	GLenum result = (GLenum)0;
	if (local_entry[56] != NULL) {
		result = ((GLenum (*)(void *, void *, GLuint, GLuint))local_entry[56])(context, a0, a1, a2);
	}
	gw_glClientWaitSync(a0, a1, a2, result);
	return result;
}

