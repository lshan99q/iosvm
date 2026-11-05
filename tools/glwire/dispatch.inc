#ifndef GLWIRE_AFTER_DRAW
#define GLWIRE_AFTER_DRAW(g) ((void)(g))
#endif

switch (opcode) {
	case 0: {
		/* glAlphaFunc */
		GLenum a0 = wire_get_u32(r);
		GLfloat a1 = wire_get_f32(r);
		glAlphaFunc(a0, a1);
		break;
	}
	case 1: {
		/* glBindTexture */
		GLenum a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		glBindTexture(a0, names_texture(g, a1));
		break;
	}
	case 2: {
		/* glBlendFunc */
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		glBlendFunc(a0, a1);
		break;
	}
	case 3: {
		/* glBlendFuncSeparate */
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLenum a2 = wire_get_u32(r);
		GLenum a3 = wire_get_u32(r);
		glBlendFuncSeparate(a0, a1, a2, a3);
		break;
	}
	case 4: {
		/* glBlendEquation */
		GLenum a0 = wire_get_u32(r);
		glBlendEquation(a0);
		break;
	}
	case 5: {
		/* glClear */
		GLuint a0 = wire_get_u32(r);
		glClear(a0);
		g->drawn_framebuffer = g->bound_framebuffer;
		break;
	}
	case 6: {
		/* glClearColor */
		GLfloat a0 = wire_get_f32(r);
		GLfloat a1 = wire_get_f32(r);
		GLfloat a2 = wire_get_f32(r);
		GLfloat a3 = wire_get_f32(r);
		glClearColor(a0, a1, a2, a3);
		break;
	}
	case 7: {
		/* glClearStencil */
		GLint a0 = wire_get_i32(r);
		glClearStencil(a0);
		break;
	}
	case 8: {
		/* glCopyTexSubImage2D */
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
		/* glDeleteTextures */
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		if (p1 != NULL) {
			for (GLsizei i = 0; i < a0; i++) {
				((GLuint *)p1)[i] = (GLuint)names_texture_take(g, ((GLuint *)p1)[i]);
			}
		}
		glDeleteTextures(a0, (const GLuint *)p1);
		free(p1);
		break;
	}
	case 10: {
		/* glDisable */
		GLenum a0 = wire_get_u32(r);
		glDisable(a0);
		break;
	}
	case 11: {
		/* glDrawArrays */
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLsizei a2 = wire_get_i32(r);
		g->drawn_framebuffer = g->bound_framebuffer;
		host_note_draw(g, "arrays", a0, a2);
		glDrawArrays(a0, a1, a2);
		names_attribs_release(g);
		GLWIRE_AFTER_DRAW(g);
		break;
	}
	case 12: {
		/* glDrawElements */
		GLenum a0 = wire_get_u32(r);
		GLsizei a1 = wire_get_i32(r);
		GLenum a2 = wire_get_u32(r);
		GLsizei n3 = 0;
		void *p3 = wire_take(r, &n3);
		g->drawn_framebuffer = g->bound_framebuffer;
		host_note_draw(g, "elements", a0, a1);
		glDrawElements(a0, a1, a2, p3);
		names_attribs_release(g);
		GLWIRE_AFTER_DRAW(g);
		free(p3);
		break;
	}
	case 13: {
		/* glEnable */
		GLenum a0 = wire_get_u32(r);
		glEnable(a0);
		break;
	}
	case 14: {
		/* glFinish */
		glFinish();
		break;
	}
	case 15: {
		/* glFlush */
		glFlush();
		break;
	}
	case 16: {
		/* glGenTextures */
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		GLuint *host_names = a0 > 0 ? calloc((size_t)a0, sizeof(GLuint)) : NULL;
		glGenTextures(a0, host_names);
		if (host_names != NULL && p1 != NULL) {
			for (GLsizei i = 0; i < a0; i++) {
				names_texture_put_new(g, ((GLuint *)p1)[i], host_names[i]);
			}
		}
		free(host_names);
		free(p1);
		break;
	}
	case 17: {
		/* glGetError */
		GLenum guest_result = wire_get_u32(r);
		(void)glGetError();
		(void)guest_result;
		break;
	}
	case 18: {
		/* glGetIntegerv */
		GLenum a0 = wire_get_u32(r);
		GLint g1[16];
		glGetIntegerv(a0, g1);
		break;
	}
	case 19: {
		/* glGetString */
		GLenum a0 = wire_get_u32(r);
		(void)glGetString(a0);
		break;
	}
	case 20: {
		/* glPixelStorei */
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		glPixelStorei(a0, a1);
		break;
	}
	case 21: {
		/* glScissor */
		GLint a0 = wire_get_i32(r);
		GLint a1 = wire_get_i32(r);
		GLsizei a2 = wire_get_i32(r);
		GLsizei a3 = wire_get_i32(r);
		glScissor(a0, a1, a2, a3);
		break;
	}
	case 22: {
		/* glTexImage2D */
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLint a2 = wire_get_i32(r);
		GLsizei a3 = wire_get_i32(r);
		GLsizei a4 = wire_get_i32(r);
		GLint a5 = wire_get_i32(r);
		GLenum a6 = wire_get_u32(r);
		GLenum a7 = wire_get_u32(r);
		GLsizei n8 = 0;
		void *p8 = wire_take_image(r, &n8);
		if (watch_wanted != NULL) {
			GLint watch_bound = 0;

			glGetIntegerv(GL_TEXTURE_BINDING_2D, &watch_bound);
			host_watch_upload(g, "teximage", (GLuint) watch_bound, a1, a2, a3, a4, a6, a7, p8, n8);
		}
		/* (twelfth stage: GL_ALPHA/LUMINANCE atlases, which this context will not take -- see
		 * host_legacy_teximage) */
		if (!host_legacy_teximage(a0, a1, a2, a3, a4, a5, a6, a7, p8)) {
			glTexImage2D(a0, a1, a2, a3, a4, a5, a6, a7, p8);
		}
		free(p8);
		break;
	}
	case 23: {
		/* glTexParameterf */
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLfloat a2 = wire_get_f32(r);
		glTexParameterf(a0, a1, a2);
		break;
	}
	case 24: {
		/* glTexParameteri */
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLint a2 = wire_get_i32(r);
		glTexParameteri(a0, a1, a2);
		break;
	}
	case 25: {
		/* glTexSubImage2D */
		GLenum a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLint a2 = wire_get_i32(r);
		GLint a3 = wire_get_i32(r);
		GLsizei a4 = wire_get_i32(r);
		GLsizei a5 = wire_get_i32(r);
		GLenum a6 = wire_get_u32(r);
		GLenum a7 = wire_get_u32(r);
		GLsizei n8 = 0;
		void *p8 = wire_take_image(r, &n8);
		if (watch_wanted != NULL) {
			GLint watch_bound = 0;

			glGetIntegerv(GL_TEXTURE_BINDING_2D, &watch_bound);
			host_watch_upload(g, "subimage", (GLuint) watch_bound, a1, 0, a4, a5, a6, a7, p8, n8);
		}
		glTexSubImage2D(a0, a1, a2, a3, a4, a5, a6, a7, p8);
		free(p8);
		break;
	}
	case 26: {
		/* glViewport */
		GLint a0 = wire_get_i32(r);
		GLint a1 = wire_get_i32(r);
		GLsizei a2 = wire_get_i32(r);
		GLsizei a3 = wire_get_i32(r);
		glViewport(a0, a1, a2, a3);
		break;
	}
	case 27: {
		/* glActiveTexture */
		GLenum a0 = wire_get_u32(r);
		glActiveTexture(a0);
		break;
	}
	case 28: {
		/* glCompressedTexImage2D */
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
		/* glVertexAttribPointer */
		GLuint a0 = wire_get_u32(r);
		GLint a1 = wire_get_i32(r);
		GLenum a2 = wire_get_u32(r);
		GLboolean a3 = wire_get_u8(r);
		GLsizei a4 = wire_get_i32(r);
		names_vertex_pointer(g, a0, a1, a2, a3, a4);
		break;
	}
	case 30: {
		/* glEnableVertexAttribArray */
		GLuint a0 = wire_get_u32(r);
		glEnableVertexAttribArray(a0);
		names_attrib_enabled(g, a0, 1);
		break;
	}
	case 31: {
		/* glDisableVertexAttribArray */
		GLuint a0 = wire_get_u32(r);
		glDisableVertexAttribArray(a0);
		names_attrib_enabled(g, a0, 0);
		break;
	}
	case 32: {
		/* glDeleteProgram */
		GLuint a0 = wire_get_u32(r);
		GLuint host_program = (GLuint)names_program_take(g, a0);
		glDeleteProgram(host_program);
		break;
	}
	case 33: {
		/* glCreateShader */
		GLenum a0 = wire_get_u32(r);
		GLuint guest_result = wire_get_u32(r);
		GLuint host = glCreateShader(a0);
		names_shader_put(g, (uint32_t)guest_result, host);
		break;
	}
	case 34: {
		/* glShaderSource */
		GLuint a0 = wire_get_u32(r);
		GLsizei a1 = wire_get_i32(r);
		GLchar **ss = NULL;
		if (a1 > 0) {
			ss = calloc((size_t)a1, sizeof(*ss));
			for (GLsizei i = 0; i < a1 && ss != NULL; i++) {
				uint32_t len = wire_get_u32(r);
				ss[i] = malloc((size_t)len + 1);
				if (ss[i] != NULL) {
					if (r->at + len <= r->capacity) { memcpy(ss[i], r->base + r->at, len); }
					r->at += len;
					ss[i][len] = '\0';
				}
			}
		}
		GLuint host_shader = names_shader(g, a0);
		names_shader_source(host_shader, a1, (const GLchar *const *)ss, NULL);
		if (ss != NULL) {
			for (GLsizei i = 0; i < a1; i++) { free(ss[i]); }
			free(ss);
		}
		break;
	}
	case 35: {
		/* glCompileShader */
		GLuint a0 = wire_get_u32(r);
		GLuint host_shader = names_shader(g, a0);
		glCompileShader(host_shader);
		if (host_shader != 0) {
			GLint good = 0;
			glGetShaderiv(host_shader, GL_COMPILE_STATUS, &good);
			if (!good) {
				char said[1024] = "";
				glGetShaderInfoLog(host_shader, sizeof(said), NULL, said);
				fprintf(stderr, "glwire: shader %u does not compile: %s\n", host_shader, said);
			}
		}
		break;
	}
	case 36: {
		/* glCreateProgram */
		GLuint guest_result = wire_get_u32(r);
		GLuint host = glCreateProgram();
		names_program_put(g, (uint32_t)guest_result, host);
		break;
	}
	case 37: {
		/* glAttachShader */
		GLuint a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		glAttachShader(names_program(g, a0), names_shader(g, a1));
		break;
	}
	case 38: {
		/* glLinkProgram */
		GLuint a0 = wire_get_u32(r);
		GLuint host_program = names_program(g, a0);
		glLinkProgram(host_program);
		if (host_program != 0) {
			GLint good = 0;
			glGetProgramiv(host_program, GL_LINK_STATUS, &good);
			if (!good) {
				char said[1024] = "";
				glGetProgramInfoLog(host_program, sizeof(said), NULL, said);
				fprintf(stderr, "glwire: program %u does not link: %s\n", host_program, said);
			}
		}
		break;
	}
	case 39: {
		/* glUseProgram */
		GLuint a0 = wire_get_u32(r);
		GLuint host_program = names_program(g, a0);
		glUseProgram(host_program);
		g->current_program = (uint32_t)a0;
		g->current_host_program = host_program;
		break;
	}
	case 40: {
		/* glUniform1f */
		GLint a0 = wire_get_i32(r);
		GLfloat a1 = wire_get_f32(r);
		glUniform1f(names_uniform(g, a0), a1);
		break;
	}
	case 41: {
		/* glUniform4f */
		GLint a0 = wire_get_i32(r);
		GLfloat a1 = wire_get_f32(r);
		GLfloat a2 = wire_get_f32(r);
		GLfloat a3 = wire_get_f32(r);
		GLfloat a4 = wire_get_f32(r);
		glUniform4f(names_uniform(g, a0), a1, a2, a3, a4);
		break;
	}
	case 42: {
		/* glUniform1i */
		GLint a0 = wire_get_i32(r);
		GLint a1 = wire_get_i32(r);
		glUniform1i(names_uniform(g, a0), a1);
		break;
	}
	case 43: {
		/* glUniform3fv */
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLsizei n2 = 0;
		void *p2 = wire_take(r, &n2);
		glUniform3fv(names_uniform(g, a0), a1, p2);
		free(p2);
		break;
	}
	case 44: {
		/* glUniform4fv */
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLsizei n2 = 0;
		void *p2 = wire_take(r, &n2);
		glUniform4fv(names_uniform(g, a0), a1, p2);
		free(p2);
		break;
	}
	case 45: {
		/* glUniformMatrix3fv */
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLboolean a2 = wire_get_u8(r);
		GLsizei n3 = 0;
		void *p3 = wire_take(r, &n3);
		glUniformMatrix3fv(names_uniform(g, a0), a1, a2, p3);
		free(p3);
		break;
	}
	case 46: {
		/* glUniformMatrix4fv */
		GLint a0 = wire_get_i32(r);
		GLsizei a1 = wire_get_i32(r);
		GLboolean a2 = wire_get_u8(r);
		GLsizei n3 = 0;
		void *p3 = wire_take(r, &n3);
		glUniformMatrix4fv(names_uniform(g, a0), a1, a2, p3);
		free(p3);
		break;
	}
	case 47: {
		/* glGetUniformLocation */
		GLuint a0 = wire_get_u32(r);
		char *p1 = wire_take_string(r);
		GLint guest_result = wire_get_i32(r);
		GLuint host_program = names_program(g, a0);
		GLint host_location = host_program != 0 ? glGetUniformLocation(host_program, p1) : -1;
		names_uniform_put(g, (uint32_t)a0, (GLint)guest_result, host_location);
		free(p1);
		break;
	}
	case 48: {
		/* glBindAttribLocation */
		GLuint a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		char *p2 = wire_take_string(r);
		glBindAttribLocation(names_program(g, a0), a1, p2);
		free(p2);
		break;
	}
	case 49: {
		/* glBindFramebuffer */
		GLenum a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		GLuint host = names_framebuffer(g, a1);

		glBindFramebuffer(a0, host);
		if (a0 != GL_READ_FRAMEBUFFER) {
			g->bound_framebuffer = host;
		}
		break;
	}
	case 50: {
		/* glDeleteFramebuffers */
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		if (p1 != NULL) {
			for (GLsizei i = 0; i < a0; i++) {
				((GLuint *)p1)[i] = (GLuint)names_framebuffer_take(g, ((GLuint *)p1)[i]);
			}
		}
		glDeleteFramebuffers(a0, (const GLuint *)p1);
		free(p1);
		break;
	}
	case 51: {
		/* glGenFramebuffers */
		GLsizei a0 = wire_get_i32(r);
		GLsizei n1 = 0;
		void *p1 = wire_take(r, &n1);
		GLuint *host_names = a0 > 0 ? calloc((size_t)a0, sizeof(GLuint)) : NULL;
		glGenFramebuffers(a0, host_names);
		if (host_names != NULL && p1 != NULL) {
			for (GLsizei i = 0; i < a0; i++) {
				names_framebuffer_put(g, ((GLuint *)p1)[i], host_names[i]);
			}
		}
		free(host_names);
		free(p1);
		break;
	}
	case 52: {
		/* glFramebufferTexture2D */
		GLenum a0 = wire_get_u32(r);
		GLenum a1 = wire_get_u32(r);
		GLenum a2 = wire_get_u32(r);
		GLuint a3 = wire_get_u32(r);
		GLint a4 = wire_get_i32(r);
		glFramebufferTexture2D(a0, a1, a2, names_texture(g, a3), a4);
		break;
	}
	case 53: {
		/* glGenerateMipmap */
		GLenum a0 = wire_get_u32(r);
		glGenerateMipmap(a0);
		break;
	}
	case 54: {
		/* glFenceSync */
		GLenum a0 = wire_get_u32(r);
		GLuint a1 = wire_get_u32(r);
		uint64_t guest_result = wire_get_u64(r);
		void *host_sync = (void *)glFenceSync(a0, a1);
		names_sync_put(g, (uint64_t)guest_result, (uint64_t)(uintptr_t)host_sync);
		break;
	}
	case 55: {
		/* glDeleteSync */
		uint64_t v0 = wire_get_u64(r);
		glDeleteSync((GLsync)(uintptr_t)names_sync_take(g, v0));
		break;
	}
	case 56: {
		/* glClientWaitSync */
		uint64_t v0 = wire_get_u64(r);
		GLuint a1 = wire_get_u32(r);
		GLuint a2 = wire_get_u32(r);
		GLenum guest_result = wire_get_u32(r);
		GLsync host_sync = (GLsync)(uintptr_t)names_sync(g, v0);
		(void)glClientWaitSync(host_sync, a1, a2);
		(void)guest_result;
		break;
	}
	case 201: {
		/* eagl.c's own IOSurface upload: pixels the table never saw */
		GLuint texture = wire_get_u32(r);
		GLenum target = wire_get_u32(r);
		GLint internal = wire_get_i32(r);
		GLsizei width = wire_get_i32(r);
		GLsizei height = wire_get_i32(r);
		GLenum format = wire_get_u32(r);
		GLenum type = wire_get_u32(r);
		GLsizei row_bytes = wire_get_i32(r);
		GLsizei pixel_bytes = wire_get_i32(r);
		GLsizei bytes = 0;
		void *data = wire_take_image(r, &bytes);
		printf("glwire: upload texture %u target %#x internal %#x %dx%d format %#x type %#x row %d pixel %d bytes %d\n",
		    texture, target, internal, width, height, format, type, row_bytes, pixel_bytes, bytes);
		fflush(stdout);
		names_texture_upload(g, texture, target, internal, width, height, format, type,
		    row_bytes, pixel_bytes, data);
		break;
	}
	case 200: {
		/* vertex arrays that the next draw needs (the guest sends them from its own
		 * pointers; the GL signature of glVertexAttribPointer carries no size) */
		GLuint index = wire_get_u32(r);
		GLint size = wire_get_i32(r);
		GLenum type = wire_get_u32(r);
		GLboolean normalized = wire_get_u8(r);
		GLsizei stride = wire_get_i32(r);
		GLsizei bytes = 0;
		void *data = wire_take(r, &bytes);
		names_attrib_data(g, index, size, type, normalized, stride, bytes, data);
		break;
	}
	default:
		fprintf(stderr, "glwire: opcode %u\n", opcode);
		return -1;
	}
