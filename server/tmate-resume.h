#ifndef TMTV_RESUME_H
#define TMTV_RESUME_H

/* A host-only 256-bit secret, independent 128-bit viewer capabilities. */
int tmtv_resume_tokens(const char *, char [33], char [36]);
int tmtv_alias_points_to(int, const char *, const char *);
int tmtv_alias_replace(int, const char *, const char *);
void tmtv_alias_remove_owned(int, const char *, const char *);

#endif
