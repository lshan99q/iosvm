/* The entry each slot held before it was pointed at the recorder: the local call that
 * still has to run. */
static void remote_glAlphaFunc(void *context, GLenum a0, GLfloat a1);
static void remote_glBindTexture(void *context, GLenum a0, GLuint a1);
static void remote_glBlendFunc(void *context, GLenum a0, GLenum a1);
static void remote_glBlendFuncSeparate(void *context, GLenum a0, GLenum a1, GLenum a2, GLenum a3);
static void remote_glBlendEquation(void *context, GLenum a0);
static void remote_glClear(void *context, GLuint a0);
static void remote_glClearColor(void *context, GLfloat a0, GLfloat a1, GLfloat a2, GLfloat a3);
static void remote_glClearStencil(void *context, GLint a0);
static void remote_glCopyTexSubImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLint a3, GLint a4, GLint a5, GLsizei a6, GLsizei a7);
static void remote_glDeleteTextures(void *context, GLsizei a0, const GLuint * a1);
static void remote_glDisable(void *context, GLenum a0);
static void remote_glDrawArrays(void *context, GLenum a0, GLint a1, GLsizei a2);
static void remote_glDrawElements(void *context, GLenum a0, GLsizei a1, GLenum a2, const void * a3);
static void remote_glEnable(void *context, GLenum a0);
static void remote_glFinish(void *context);
static void remote_glFlush(void *context);
static void remote_glGenTextures(void *context, GLsizei a0, const GLuint * a1);
static GLenum remote_glGetError(void *context);
static void remote_glGetIntegerv(void *context, GLenum a0, GLint * a1);
static void * remote_glGetString(void *context, GLenum a0);
static void remote_glPixelStorei(void *context, GLenum a0, GLint a1);
static void remote_glScissor(void *context, GLint a0, GLint a1, GLsizei a2, GLsizei a3);
static void remote_glTexImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLsizei a3, GLsizei a4, GLint a5, GLenum a6, GLenum a7, const void * a8);
static void remote_glTexParameterf(void *context, GLenum a0, GLenum a1, GLfloat a2);
static void remote_glTexParameteri(void *context, GLenum a0, GLenum a1, GLint a2);
static void remote_glTexSubImage2D(void *context, GLenum a0, GLint a1, GLint a2, GLint a3, GLsizei a4, GLsizei a5, GLenum a6, GLenum a7, const void * a8);
static void remote_glViewport(void *context, GLint a0, GLint a1, GLsizei a2, GLsizei a3);
static void remote_glActiveTexture(void *context, GLenum a0);
static void remote_glCompressedTexImage2D(void *context, GLenum a0, GLint a1, GLenum a2, GLsizei a3, GLsizei a4, GLint a5, GLsizei a6, const void * a7);
static void remote_glVertexAttribPointer(void *context, GLuint a0, GLint a1, GLenum a2, GLboolean a3, GLsizei a4, const void * a5);
static void remote_glEnableVertexAttribArray(void *context, GLuint a0);
static void remote_glDisableVertexAttribArray(void *context, GLuint a0);
static void remote_glDeleteProgram(void *context, GLuint a0);
static GLuint remote_glCreateShader(void *context, GLenum a0);
static void remote_glShaderSource(void *context, GLuint a0, GLsizei a1, const GLchar *const * a2, const GLint * a3);
static void remote_glCompileShader(void *context, GLuint a0);
static GLuint remote_glCreateProgram(void *context);
static void remote_glAttachShader(void *context, GLuint a0, GLuint a1);
static void remote_glLinkProgram(void *context, GLuint a0);
static void remote_glUseProgram(void *context, GLuint a0);
static void remote_glUniform1f(void *context, GLint a0, GLfloat a1);
static void remote_glUniform4f(void *context, GLint a0, GLfloat a1, GLfloat a2, GLfloat a3, GLfloat a4);
static void remote_glUniform1i(void *context, GLint a0, GLint a1);
static void remote_glUniform3fv(void *context, GLint a0, GLsizei a1, const GLfloat * a2);
static void remote_glUniform4fv(void *context, GLint a0, GLsizei a1, const GLfloat * a2);
static void remote_glUniformMatrix3fv(void *context, GLint a0, GLsizei a1, GLboolean a2, const GLfloat * a3);
static void remote_glUniformMatrix4fv(void *context, GLint a0, GLsizei a1, GLboolean a2, const GLfloat * a3);
static GLint remote_glGetUniformLocation(void *context, GLuint a0, const GLchar * a1);
static void remote_glBindAttribLocation(void *context, GLuint a0, GLuint a1, const GLchar * a2);
static void remote_glBindFramebuffer(void *context, GLenum a0, GLuint a1);
static void remote_glDeleteFramebuffers(void *context, GLsizei a0, const GLuint * a1);
static void remote_glGenFramebuffers(void *context, GLsizei a0, const GLuint * a1);
static void remote_glFramebufferTexture2D(void *context, GLenum a0, GLenum a1, GLenum a2, GLuint a3, GLint a4);
static void remote_glGenerateMipmap(void *context, GLenum a0);
static void * remote_glFenceSync(void *context, GLenum a0, GLuint a1);
static void remote_glDeleteSync(void *context, void * a0);
static GLenum remote_glClientWaitSync(void *context, void * a0, GLuint a1, GLuint a2);

static void *local_entry[57];
/* The table index of every recorded call, in spec order: eagl_remote.c uses it to put
 * a slot back for the bisection switch glwire_slots=MASK. */
static const unsigned short remote_slot_offset[] = {
	0x2,     /* 0 glAlphaFunc */
	0x6,     /* 1 glBindTexture */
	0x8,     /* 2 glBlendFunc */
	0x151,     /* 3 glBlendFuncSeparate */
	0x153,     /* 4 glBlendEquation */
	0xb,     /* 5 glClear */
	0xd,     /* 6 glClearColor */
	0x10,     /* 7 glClearStencil */
	0x39,     /* 8 glCopyTexSubImage2D */
	0x3c,     /* 9 glDeleteTextures */
	0x40,     /* 10 glDisable */
	0x42,     /* 11 glDrawArrays */
	0x44,     /* 12 glDrawElements */
	0x49,     /* 13 glEnable */
	0x5a,     /* 14 glFinish */
	0x5b,     /* 15 glFlush */
	0x63,     /* 16 glGenTextures */
	0x67,     /* 17 glGetError */
	0x69,     /* 18 glGetIntegerv */
	0x76,     /* 19 glGetString */
	0xc4,     /* 20 glPixelStorei */
	0xfc,     /* 21 glScissor */
	0x12e,     /* 22 glTexImage2D */
	0x12f,     /* 23 glTexParameterf */
	0x131,     /* 24 glTexParameteri */
	0x134,     /* 25 glTexSubImage2D */
	0x150,     /* 26 glViewport */
	0x157,     /* 27 glActiveTexture */
	0x17d,     /* 28 glCompressedTexImage2D */
	0x200,     /* 29 glVertexAttribPointer */
	0x201,     /* 30 glEnableVertexAttribArray */
	0x202,     /* 31 glDisableVertexAttribArray */
	0x250,     /* 32 glDeleteProgram */
	0x253,     /* 33 glCreateShader */
	0x254,     /* 34 glShaderSource */
	0x255,     /* 35 glCompileShader */
	0x256,     /* 36 glCreateProgram */
	0x257,     /* 37 glAttachShader */
	0x258,     /* 38 glLinkProgram */
	0x259,     /* 39 glUseProgram */
	0x25b,     /* 40 glUniform1f */
	0x25e,     /* 41 glUniform4f */
	0x25f,     /* 42 glUniform1i */
	0x265,     /* 43 glUniform3fv */
	0x266,     /* 44 glUniform4fv */
	0x26c,     /* 45 glUniformMatrix3fv */
	0x26d,     /* 46 glUniformMatrix4fv */
	0x272,     /* 47 glGetUniformLocation */
	0x277,     /* 48 glBindAttribLocation */
	0x2a1,     /* 49 glBindFramebuffer */
	0x2a2,     /* 50 glDeleteFramebuffers */
	0x2a3,     /* 51 glGenFramebuffers */
	0x2a6,     /* 52 glFramebufferTexture2D */
	0x2aa,     /* 53 glGenerateMipmap */
	0x30c,     /* 54 glFenceSync */
	0x30e,     /* 55 glDeleteSync */
	0x30f,     /* 56 glClientWaitSync */
};

static void
eagl_fill_table_remote(void **table)
{
	local_entry[0] = table[0x10 / 8];
	table[0x10 / 8] = (void *)remote_glAlphaFunc;
	local_entry[1] = table[0x30 / 8];
	table[0x30 / 8] = (void *)remote_glBindTexture;
	local_entry[2] = table[0x40 / 8];
	table[0x40 / 8] = (void *)remote_glBlendFunc;
	local_entry[3] = table[0xa88 / 8];
	table[0xa88 / 8] = (void *)remote_glBlendFuncSeparate;
	local_entry[4] = table[0xa98 / 8];
	table[0xa98 / 8] = (void *)remote_glBlendEquation;
	local_entry[5] = table[0x58 / 8];
	table[0x58 / 8] = (void *)remote_glClear;
	local_entry[6] = table[0x68 / 8];
	table[0x68 / 8] = (void *)remote_glClearColor;
	local_entry[7] = table[0x80 / 8];
	table[0x80 / 8] = (void *)remote_glClearStencil;
	local_entry[8] = table[0x1c8 / 8];
	table[0x1c8 / 8] = (void *)remote_glCopyTexSubImage2D;
	local_entry[9] = table[0x1e0 / 8];
	table[0x1e0 / 8] = (void *)remote_glDeleteTextures;
	local_entry[10] = table[0x200 / 8];
	table[0x200 / 8] = (void *)remote_glDisable;
	local_entry[11] = table[0x210 / 8];
	table[0x210 / 8] = (void *)remote_glDrawArrays;
	local_entry[12] = table[0x220 / 8];
	table[0x220 / 8] = (void *)remote_glDrawElements;
	local_entry[13] = table[0x248 / 8];
	table[0x248 / 8] = (void *)remote_glEnable;
	local_entry[14] = table[0x2d0 / 8];
	table[0x2d0 / 8] = (void *)remote_glFinish;
	local_entry[15] = table[0x2d8 / 8];
	table[0x2d8 / 8] = (void *)remote_glFlush;
	local_entry[16] = table[0x318 / 8];
	table[0x318 / 8] = (void *)remote_glGenTextures;
	local_entry[17] = table[0x338 / 8];
	table[0x338 / 8] = (void *)remote_glGetError;
	local_entry[18] = table[0x348 / 8];
	table[0x348 / 8] = (void *)remote_glGetIntegerv;
	local_entry[19] = table[0x3b0 / 8];
	table[0x3b0 / 8] = (void *)remote_glGetString;
	local_entry[20] = table[0x620 / 8];
	table[0x620 / 8] = (void *)remote_glPixelStorei;
	local_entry[21] = table[0x7e0 / 8];
	table[0x7e0 / 8] = (void *)remote_glScissor;
	local_entry[22] = table[0x970 / 8];
	table[0x970 / 8] = (void *)remote_glTexImage2D;
	local_entry[23] = table[0x978 / 8];
	table[0x978 / 8] = (void *)remote_glTexParameterf;
	local_entry[24] = table[0x988 / 8];
	table[0x988 / 8] = (void *)remote_glTexParameteri;
	local_entry[25] = table[0x9a0 / 8];
	table[0x9a0 / 8] = (void *)remote_glTexSubImage2D;
	local_entry[26] = table[0xa80 / 8];
	table[0xa80 / 8] = (void *)remote_glViewport;
	local_entry[27] = table[0xab8 / 8];
	table[0xab8 / 8] = (void *)remote_glActiveTexture;
	local_entry[28] = table[0xbe8 / 8];
	table[0xbe8 / 8] = (void *)remote_glCompressedTexImage2D;
	local_entry[29] = table[0x1000 / 8];
	table[0x1000 / 8] = (void *)remote_glVertexAttribPointer;
	local_entry[30] = table[0x1008 / 8];
	table[0x1008 / 8] = (void *)remote_glEnableVertexAttribArray;
	local_entry[31] = table[0x1010 / 8];
	table[0x1010 / 8] = (void *)remote_glDisableVertexAttribArray;
	local_entry[32] = table[0x1280 / 8];
	table[0x1280 / 8] = (void *)remote_glDeleteProgram;
	local_entry[33] = table[0x1298 / 8];
	table[0x1298 / 8] = (void *)remote_glCreateShader;
	local_entry[34] = table[0x12a0 / 8];
	table[0x12a0 / 8] = (void *)remote_glShaderSource;
	local_entry[35] = table[0x12a8 / 8];
	table[0x12a8 / 8] = (void *)remote_glCompileShader;
	local_entry[36] = table[0x12b0 / 8];
	table[0x12b0 / 8] = (void *)remote_glCreateProgram;
	local_entry[37] = table[0x12b8 / 8];
	table[0x12b8 / 8] = (void *)remote_glAttachShader;
	local_entry[38] = table[0x12c0 / 8];
	table[0x12c0 / 8] = (void *)remote_glLinkProgram;
	local_entry[39] = table[0x12c8 / 8];
	table[0x12c8 / 8] = (void *)remote_glUseProgram;
	local_entry[40] = table[0x12d8 / 8];
	table[0x12d8 / 8] = (void *)remote_glUniform1f;
	local_entry[41] = table[0x12f0 / 8];
	table[0x12f0 / 8] = (void *)remote_glUniform4f;
	local_entry[42] = table[0x12f8 / 8];
	table[0x12f8 / 8] = (void *)remote_glUniform1i;
	local_entry[43] = table[0x1328 / 8];
	table[0x1328 / 8] = (void *)remote_glUniform3fv;
	local_entry[44] = table[0x1330 / 8];
	table[0x1330 / 8] = (void *)remote_glUniform4fv;
	local_entry[45] = table[0x1360 / 8];
	table[0x1360 / 8] = (void *)remote_glUniformMatrix3fv;
	local_entry[46] = table[0x1368 / 8];
	table[0x1368 / 8] = (void *)remote_glUniformMatrix4fv;
	local_entry[47] = table[0x1390 / 8];
	table[0x1390 / 8] = (void *)remote_glGetUniformLocation;
	local_entry[48] = table[0x13b8 / 8];
	table[0x13b8 / 8] = (void *)remote_glBindAttribLocation;
	local_entry[49] = table[0x1508 / 8];
	table[0x1508 / 8] = (void *)remote_glBindFramebuffer;
	local_entry[50] = table[0x1510 / 8];
	table[0x1510 / 8] = (void *)remote_glDeleteFramebuffers;
	local_entry[51] = table[0x1518 / 8];
	table[0x1518 / 8] = (void *)remote_glGenFramebuffers;
	local_entry[52] = table[0x1530 / 8];
	table[0x1530 / 8] = (void *)remote_glFramebufferTexture2D;
	local_entry[53] = table[0x1550 / 8];
	table[0x1550 / 8] = (void *)remote_glGenerateMipmap;
	local_entry[54] = table[0x1860 / 8];
	table[0x1860 / 8] = (void *)remote_glFenceSync;
	local_entry[55] = table[0x1870 / 8];
	table[0x1870 / 8] = (void *)remote_glDeleteSync;
	local_entry[56] = table[0x1878 / 8];
	table[0x1878 / 8] = (void *)remote_glClientWaitSync;
}
