/* glAlphaFunc */
void gw_glAlphaFunc(GLenum a0, GLfloat a1)
{
	wire_open(&w, 0);
	wire_u32(&w, a0);
	wire_f32(&w, a1);
	wire_close(&w);
}

/* glBindTexture */
void gw_glBindTexture(GLenum a0, GLuint a1)
{
	wire_open(&w, 1);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

/* glBlendFunc */
void gw_glBlendFunc(GLenum a0, GLenum a1)
{
	wire_open(&w, 2);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

/* glBlendFuncSeparate */
void gw_glBlendFuncSeparate(GLenum a0, GLenum a1, GLenum a2, GLenum a3)
{
	wire_open(&w, 3);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_u32(&w, a2);
	wire_u32(&w, a3);
	wire_close(&w);
}

/* glBlendEquation */
void gw_glBlendEquation(GLenum a0)
{
	wire_open(&w, 4);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glClear */
void gw_glClear(GLuint a0)
{
	wire_open(&w, 5);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glClearColor */
void gw_glClearColor(GLfloat a0, GLfloat a1, GLfloat a2, GLfloat a3)
{
	wire_open(&w, 6);
	wire_f32(&w, a0);
	wire_f32(&w, a1);
	wire_f32(&w, a2);
	wire_f32(&w, a3);
	wire_close(&w);
}

/* glClearStencil */
void gw_glClearStencil(GLint a0)
{
	wire_open(&w, 7);
	wire_i32(&w, a0);
	wire_close(&w);
}

/* glCopyTexSubImage2D */
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

/* glDeleteTextures */
void gw_glDeleteTextures(GLsizei a0, const GLuint * a1)
{
	wire_open(&w, 9);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, a0 > 0 ? (GLsizei)(a0 * 4) : 0);
	wire_close(&w);
}

/* glDisable */
void gw_glDisable(GLenum a0)
{
	wire_open(&w, 10);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glDrawArrays */
void gw_glDrawArrays(GLenum a0, GLint a1, GLsizei a2)
{
	wire_open(&w, 11);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_close(&w);
}

/* glDrawElements */
void gw_glDrawElements(GLenum a0, GLsizei a1, GLenum a2, const void * a3)
{
	wire_open(&w, 12);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_u32(&w, a2);
	wire_bytes(&w, a3, wire_index_bytes(a1, a2));
	wire_close(&w);
}

/* glEnable */
void gw_glEnable(GLenum a0)
{
	wire_open(&w, 13);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glFinish */
void gw_glFinish()
{
	wire_open(&w, 14);

	wire_close(&w);
}

/* glFlush */
void gw_glFlush()
{
	wire_open(&w, 15);

	wire_close(&w);
}

/* glGenTextures */
void gw_glGenTextures(GLsizei a0, const GLuint * a1)
{
	wire_open(&w, 16);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, a0 > 0 ? (GLsizei)(a0 * 4) : 0);
	wire_close(&w);
}

/* glGetError -> E */
void gw_glGetError(GLenum result)
{
	wire_open(&w, 17);
	wire_u32(&w, result);
	wire_close(&w);
}

/* glGetIntegerv */
void gw_glGetIntegerv(GLenum a0, GLint * a1)
{
	wire_open(&w, 18);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glGetString -> P */
void gw_glGetString(GLenum a0, void * result)
{
	wire_open(&w, 19);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glPixelStorei */
void gw_glPixelStorei(GLenum a0, GLint a1)
{
	wire_open(&w, 20);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_close(&w);
}

/* glScissor */
void gw_glScissor(GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	wire_open(&w, 21);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_i32(&w, a3);
	wire_close(&w);
}

/* glTexImage2D */
void gw_glTexImage2D(GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5, GLenum a6, GLenum a7, const void * a8)
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
	wire_bytes(&w, a8, wire_image_bytes(a3, a4, a6, a7));
	wire_close(&w);
}

/* glTexParameterf */
void gw_glTexParameterf(GLenum a0, GLenum a1, GLfloat a2)
{
	wire_open(&w, 23);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_f32(&w, a2);
	wire_close(&w);
}

/* glTexParameteri */
void gw_glTexParameteri(GLenum a0, GLenum a1, GLint a2)
{
	wire_open(&w, 24);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_i32(&w, a2);
	wire_close(&w);
}

/* glTexSubImage2D */
void gw_glTexSubImage2D(GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5, GLenum a6, GLenum a7, const void * a8)
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
	wire_bytes(&w, a8, wire_image_bytes(a4, a5, a6, a7));
	wire_close(&w);
}

/* glViewport */
void gw_glViewport(GLint a0, GLint a1, GLsizei a2, GLsizei a3)
{
	wire_open(&w, 26);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_i32(&w, a2);
	wire_i32(&w, a3);
	wire_close(&w);
}

/* glActiveTexture */
void gw_glActiveTexture(GLenum a0)
{
	wire_open(&w, 27);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glCompressedTexImage2D */
void gw_glCompressedTexImage2D(GLenum a0, GLint a1, GLenum a2, GLsizei a3, GLsizei a4, GLint a5, GLsizei a6, const void * a7)
{
	wire_open(&w, 28);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_u32(&w, a2);
	wire_i32(&w, a3);
	wire_i32(&w, a4);
	wire_i32(&w, a5);
	wire_i32(&w, a6);
	wire_bytes(&w, a7, a6 > 0 ? a6 : 0);
	wire_close(&w);
}

/* glVertexAttribPointer */
void gw_glVertexAttribPointer(GLuint a0, GLint a1, GLenum a2, GLboolean a3, GLsizei a4, const void * a5)
{
	wire_open(&w, 29);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	wire_u32(&w, a2);
	wire_u8(&w, a3);
	wire_i32(&w, a4);
	wire_close(&w);
}

/* glEnableVertexAttribArray */
void gw_glEnableVertexAttribArray(GLuint a0)
{
	wire_open(&w, 30);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glDisableVertexAttribArray */
void gw_glDisableVertexAttribArray(GLuint a0)
{
	wire_open(&w, 31);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glDeleteProgram */
void gw_glDeleteProgram(GLuint a0)
{
	wire_open(&w, 32);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glCreateShader -> U */
void gw_glCreateShader(GLenum a0, GLuint result)
{
	wire_open(&w, 33);
	wire_u32(&w, a0);
	wire_u32(&w, result);
	wire_close(&w);
}

/* glShaderSource */
void gw_glShaderSource(GLuint a0, GLsizei a1, const GLchar *const * a2, const GLint * a3)
{
	wire_open(&w, 34);
	wire_u32(&w, a0);
	wire_i32(&w, a1);
	for (GLsizei i = 0; i < a1; i++) {
		const GLchar *text = a2 != NULL ? a2[i] : NULL;
		GLint length = a3 != NULL ? a3[i] : -1;
		if (text == NULL) { wire_u32(&w, 0); continue; }
		if (length < 0) { length = (GLint)strlen(text); }
		wire_u32(&w, (uint32_t)length);
		if (length > 0) { wire_put(&w, text, (size_t)length); }
	}
	wire_close(&w);
}

/* glCompileShader */
void gw_glCompileShader(GLuint a0)
{
	wire_open(&w, 35);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glCreateProgram -> U */
void gw_glCreateProgram(GLuint result)
{
	wire_open(&w, 36);
	wire_u32(&w, result);
	wire_close(&w);
}

/* glAttachShader */
void gw_glAttachShader(GLuint a0, GLuint a1)
{
	wire_open(&w, 37);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

/* glLinkProgram */
void gw_glLinkProgram(GLuint a0)
{
	wire_open(&w, 38);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glUseProgram */
void gw_glUseProgram(GLuint a0)
{
	wire_open(&w, 39);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glUniform1f */
void gw_glUniform1f(GLint a0, GLfloat a1)
{
	wire_open(&w, 40);
	wire_i32(&w, a0);
	wire_f32(&w, a1);
	wire_close(&w);
}

/* glUniform4f */
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

/* glUniform1i */
void gw_glUniform1i(GLint a0, GLint a1)
{
	wire_open(&w, 42);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_close(&w);
}

/* glUniform3fv */
void gw_glUniform3fv(GLint a0, GLsizei a1, const GLfloat * a2)
{
	wire_open(&w, 43);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_bytes(&w, a2, a1 > 0 ? (GLsizei)(a1 * 12) : 0);
	wire_close(&w);
}

/* glUniform4fv */
void gw_glUniform4fv(GLint a0, GLsizei a1, const GLfloat * a2)
{
	wire_open(&w, 44);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_bytes(&w, a2, a1 > 0 ? (GLsizei)(a1 * 16) : 0);
	wire_close(&w);
}

/* glUniformMatrix3fv */
void gw_glUniformMatrix3fv(GLint a0, GLsizei a1, GLboolean a2, const GLfloat * a3)
{
	wire_open(&w, 45);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_u8(&w, a2);
	wire_bytes(&w, a3, a1 > 0 ? (GLsizei)(a1 * 36) : 0);
	wire_close(&w);
}

/* glUniformMatrix4fv */
void gw_glUniformMatrix4fv(GLint a0, GLsizei a1, GLboolean a2, const GLfloat * a3)
{
	wire_open(&w, 46);
	wire_i32(&w, a0);
	wire_i32(&w, a1);
	wire_u8(&w, a2);
	wire_bytes(&w, a3, a1 > 0 ? (GLsizei)(a1 * 64) : 0);
	wire_close(&w);
}

/* glGetUniformLocation -> I */
void gw_glGetUniformLocation(GLuint a0, const GLchar * a1, GLint result)
{
	wire_open(&w, 47);
	wire_u32(&w, a0);
	wire_string(&w, a1);
	wire_i32(&w, result);
	wire_close(&w);
}

/* glBindAttribLocation */
void gw_glBindAttribLocation(GLuint a0, GLuint a1, const GLchar * a2)
{
	wire_open(&w, 48);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_string(&w, a2);
	wire_close(&w);
}

/* glBindFramebuffer */
void gw_glBindFramebuffer(GLenum a0, GLuint a1)
{
	wire_open(&w, 49);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_close(&w);
}

/* glDeleteFramebuffers */
void gw_glDeleteFramebuffers(GLsizei a0, const GLuint * a1)
{
	wire_open(&w, 50);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, a0 > 0 ? (GLsizei)(a0 * 4) : 0);
	wire_close(&w);
}

/* glGenFramebuffers */
void gw_glGenFramebuffers(GLsizei a0, const GLuint * a1)
{
	wire_open(&w, 51);
	wire_i32(&w, a0);
	wire_bytes(&w, a1, a0 > 0 ? (GLsizei)(a0 * 4) : 0);
	wire_close(&w);
}

/* glFramebufferTexture2D */
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

/* glGenerateMipmap */
void gw_glGenerateMipmap(GLenum a0)
{
	wire_open(&w, 53);
	wire_u32(&w, a0);
	wire_close(&w);
}

/* glFenceSync -> q */
void gw_glFenceSync(GLenum a0, GLuint a1, void * result)
{
	wire_open(&w, 54);
	wire_u32(&w, a0);
	wire_u32(&w, a1);
	wire_u64(&w, (uint64_t)(uintptr_t)result);
	wire_close(&w);
}

/* glDeleteSync */
void gw_glDeleteSync(void * a0)
{
	wire_open(&w, 55);
	wire_u64(&w, (uint64_t)(uintptr_t)a0);
	wire_close(&w);
}

/* glClientWaitSync -> E */
void gw_glClientWaitSync(void * a0, GLuint a1, GLuint a2, GLenum result)
{
	wire_open(&w, 56);
	wire_u64(&w, (uint64_t)(uintptr_t)a0);
	wire_u32(&w, a1);
	wire_u32(&w, a2);
	wire_u32(&w, result);
	wire_close(&w);
}

