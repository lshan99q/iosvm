switch (opcode) {
	case 0: {
		GLenum a0 = wire_get_u32(r);
		GLfloat a1 = wire_get_f32(r);
		glAlphaFunc(a0, a1);
		break;
	}

	case 1: {
		GLenum a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		glBindTexture(a0, a1);
		break;
	}

	case 2: {
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		glBlendFunc(a0, a1);
		break;
	}

	case 3: {
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLenum a2 = wire_get_u32(r);
		GLenum a3 = wire_get_u32(r);
		glBlendFuncSeparate(a0, a1, a2, a3);
		break;
	}

	case 4: {
		GLenum a0 = wire_get_u32(r);
		glBlendEquation(a0);
		break;
	}

	case 5: {
		GLuint a0 = wire_get_u32(r);
		glClear(a0);
		break;
	}

	case 6: {
		GLfloat a0 = wire_get_f32(r);
		GLfloat a1 = wire_get_f32(r);
		GLfloat a2 = wire_get_f32(r);
		GLfloat a3 = wire_get_f32(r);
		glClearColor(a0, a1, a2, a3);
		break;
	}

	case 7: {
		GLint a0 = wire_get_i32(r);
		glClearStencil(a0);
		break;
	}

	case 8: {
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLint a2 = wire_get_i32(r);
		GLint a3 = wire_get_i32(r);
		GLint a4 = wire_get_i32(r);
		GLint a5 = wire_get_i32(r);
		GLsizei a6 = wire_get_i32(r);
		GLsizei a7 = wire_get_i32(r);
		glCopyTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7);
		break;
	}

	case 9: {
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		glDeleteTextures(a0, p1);
		free(p1);
		break;
	}

	case 10: {
		GLenum a0 = wire_get_u32(r);
		glDisable(a0);
		break;
	}

	case 11: {
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLsizei a2 = wire_get_i32(r);
		glDrawArrays(a0, a1, a2);
		break;
	}

	case 12: {
		GLenum a0 = wire_get_u32(r);
		GLsizei a1 = wire_get_i32(r);
		GLenum a2 = wire_get_u32(r);
		GLsizei n3 = 0;
		void *p3 = wire_take(r, &n3);
		glDrawElements(a0, a1, a2, p3);
		free(p3);
		break;
	}

	case 13: {
		GLenum a0 = wire_get_u32(r);
		glEnable(a0);
		break;
	}

	case 14: {
		/* no arguments */
		glFinish();
		break;
	}

	case 15: {
		/* no arguments */
		glFlush();
		break;
	}

	case 16: {
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		glGenTextures(a0, p1);
		free(p1);
		break;
	}

	case 17: {
		/* no arguments */
		(void)glGetError();
		break;
	}

	case 18: {
		GLenum a0 = wire_get_u32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		glGetIntegerv(a0, p1);
		free(p1);
		break;
	}

	case 19: {
		GLenum a0 = wire_get_u32(r);
		(void)glGetString(a0);
		break;
	}

	case 20: {
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		glPixelStorei(a0, a1);
		break;
	}

	case 21: {
		GLint a0 = wire_get_i32(r);
		GLint a1 = wire_get_i32(r);
		GLsizei a2 = wire_get_i32(r);
		GLsizei a3 = wire_get_i32(r);
		glScissor(a0, a1, a2, a3);
		break;
	}

	case 22: {
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLint a2 = wire_get_i32(r);
		GLsizei a3 = wire_get_i32(r);
		GLsizei a4 = wire_get_i32(r);
		GLint a5 = wire_get_i32(r);
		GLenum a6 = wire_get_u32(r);
		GLenum a7 = wire_get_u32(r);
		GLsizei n8 = 0;
		void *p8 = wire_take(r, &n8);
		glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, p8);
		free(p8);
		break;
	}

	case 23: {
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLfloat a2 = wire_get_f32(r);
		glTexParameterf(a0, a1, a2);
		break;
	}

	case 24: {
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLint a2 = wire_get_i32(r);
		glTexParameteri(a0, a1, a2);
		break;
	}

	case 25: {
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLint a2 = wire_get_i32(r);
		GLint a3 = wire_get_i32(r);
		GLsizei a4 = wire_get_i32(r);
		GLsizei a5 = wire_get_i32(r);
		GLenum a6 = wire_get_u32(r);
		GLenum a7 = wire_get_u32(r);
		GLsizei n8 = 0;
		void *p8 = wire_take(r, &n8);
		glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, p8);
		free(p8);
		break;
	}

	case 26: {
		GLint a0 = wire_get_i32(r);
		GLint a1 = wire_get_i32(r);
		GLsizei a2 = wire_get_i32(r);
		GLsizei a3 = wire_get_i32(r);
		glViewport(a0, a1, a2, a3);
		break;
	}

	case 27: {
		GLenum a0 = wire_get_u32(r);
		glActiveTexture(a0);
		break;
	}

	case 28: {
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLenum a2 = wire_get_u32(r);
		GLsizei a3 = wire_get_i32(r);
		GLsizei a4 = wire_get_i32(r);
		GLint a5 = wire_get_i32(r);
		GLsizei a6 = wire_get_i32(r);
		GLsizei n7 = 0;
		void *p7 = wire_take(r, &n7);
		glCompressedTexImage2D(a0, a1, a2, a3, a4, a5, a6, p7);
		free(p7);
		break;
	}

	case 29: {
		GLuint a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLenum a2 = wire_get_u32(r);
		GLboolean a3 = wire_get_u8(r);
		GLsizei a4 = wire_get_i32(r);
		GLsizei n5 = 0;
		void *p5 = wire_take(r, &n5);
		glVertexAttribPointer(a0, a1, a2, a3, a4, p5);
		free(p5);
		break;
	}

	case 30: {
		GLuint a0 = wire_get_u32(r);
		glEnableVertexAttribArray(a0);
		break;
	}

	case 31: {
		GLuint a0 = wire_get_u32(r);
		glDisableVertexAttribArray(a0);
		break;
	}

	case 32: {
		GLuint a0 = wire_get_u32(r);
		glDeleteProgram(a0);
		break;
	}

	case 33: {
		GLenum a0 = wire_get_u32(r);
		(void)glCreateShader(a0);
		break;
	}

	case 34: {
		GLuint a0 = wire_get_u32(r);
		GLsizei a1 = wire_get_i32(r);
		GLsizei n2 = 0;
		void *p2 = wire_take(r, &n2);
		GLsizei n3 = 0;
		void *p3 = wire_take(r, &n3);
		glShaderSource(a0, a1, p2, p3);
		free(p2);
		free(p3);
		break;
	}

	case 35: {
		GLuint a0 = wire_get_u32(r);
		glCompileShader(a0);
		break;
	}

	case 36: {
		/* no arguments */
		(void)glCreateProgram();
		break;
	}

	case 37: {
		GLuint a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		glAttachShader(a0, a1);
		break;
	}

	case 38: {
		GLuint a0 = wire_get_u32(r);
		glLinkProgram(a0);
		break;
	}

	case 39: {
		GLuint a0 = wire_get_u32(r);
		glUseProgram(a0);
		break;
	}

	case 40: {
		GLint a0 = wire_get_i32(r);
		GLfloat a1 = wire_get_f32(r);
		glUniform1f(a0, a1);
		break;
	}

	case 41: {
		GLint a0 = wire_get_i32(r);
		GLfloat a1 = wire_get_f32(r);
		GLfloat a2 = wire_get_f32(r);
		GLfloat a3 = wire_get_f32(r);
		GLfloat a4 = wire_get_f32(r);
		glUniform4f(a0, a1, a2, a3, a4);
		break;
	}

	case 42: {
		GLint a0 = wire_get_i32(r);
		GLint a1 = wire_get_i32(r);
		glUniform1i(a0, a1);
		break;
	}

	case 43: {
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLsizei n2 = 0;
		void *p2 = wire_take(r, &n2);
		glUniform3fv(a0, a1, p2);
		free(p2);
		break;
	}

	case 44: {
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLsizei n2 = 0;
		void *p2 = wire_take(r, &n2);
		glUniform4fv(a0, a1, p2);
		free(p2);
		break;
	}

	case 45: {
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLboolean a2 = wire_get_u8(r);
		GLsizei n3 = 0;
		void *p3 = wire_take(r, &n3);
		glUniformMatrix3fv(a0, a1, a2, p3);
		free(p3);
		break;
	}

	case 46: {
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLboolean a2 = wire_get_u8(r);
		GLsizei n3 = 0;
		void *p3 = wire_take(r, &n3);
		glUniformMatrix4fv(a0, a1, a2, p3);
		free(p3);
		break;
	}

	case 47: {
		GLuint a0 = wire_get_u32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		(void)glGetUniformLocation(a0, p1);
		free(p1);
		break;
	}

	case 48: {
		GLuint a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		GLsizei n2 = 0;
		void *p2 = wire_take(r, &n2);
		glBindAttribLocation(a0, a1, p2);
		free(p2);
		break;
	}

	case 49: {
		GLenum a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		glBindFramebuffer(a0, a1);
		break;
	}

	case 50: {
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		glDeleteFramebuffers(a0, p1);
		free(p1);
		break;
	}

	case 51: {
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		glGenFramebuffers(a0, p1);
		free(p1);
		break;
	}

	case 52: {
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLenum a2 = wire_get_u32(r);
		GLuint a3 = wire_get_u32(r);
		GLint a4 = wire_get_i32(r);
		glFramebufferTexture2D(a0, a1, a2, a3, a4);
		break;
	}

	case 53: {
		GLenum a0 = wire_get_u32(r);
		glGenerateMipmap(a0);
		break;
	}

	case 54: {
		GLenum a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		(void)glFenceSync(a0, a1);
		break;
	}

	case 55: {
		GLsizei n0 = 0;
		void *p0 = wire_take(r, &n0);
		glDeleteSync(p0);
		free(p0);
		break;
	}

	case 56: {
		GLsizei n0 = 0;
		void *p0 = wire_take(r, &n0);
		GLuint a1 = wire_get_u32(r);
		GLuint a2 = wire_get_u32(r);
		(void)glClientWaitSync(p0, a1, a2);
		free(p0);
		break;
	}

	default:
		fprintf(stderr, "glwire: opcode %u\n", opcode);
		return -1;
	}
