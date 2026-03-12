#include "tmate.h"

void tmate_set_env(struct tmate_session *session,
		   const char *name, const char *value)
{
	struct tmate_env *te;

	TAILQ_FOREACH(te, &session->env, entry) {
		if (!strcmp(te->name, name)) {
			free(te->value);
			te->value = xstrdup(value);
			return;
		}
	}

	te = xmalloc(sizeof(*te));
	te->name = xstrdup(name);
	te->value = xstrdup(value);
	TAILQ_INSERT_HEAD(&session->env, te, entry);
}

void tmate_format(struct tmate_session *session, struct format_tree *ft)
{
	struct tmate_env *te;

	TAILQ_FOREACH(te, &session->env, entry) {
		format_add(ft, te->name, "%s", te->value);
	}
}
