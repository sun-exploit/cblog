#include <sys/stat.h>
#include <ctype.h>
#include <stdio.h>
#include <libgen.h>
#include "buffer.h"
#include "markdown.h"
#include "renderers.h"
#include <stdbool.h>
#include <stdlib.h>
#include <err.h>
#include <limits.h>
#include <cdb.h>
#include <inttypes.h>
#include <unistd.h>
#include <fcntl.h>
#include "cblogctl.h"
#include "cblog_common.h"
#include "cblog_utils.h"

/* path the the CDB database file */
char	cblog_cdb[PATH_MAX];
char	cblog_cdb_tmp[PATH_MAX];

void
cblogctl_list(void)
{
	sqlite3 *sqlite;
	sqlite3_stmt *stmt;

	sqlite3_initialize();
	sqlite3_open(cblog_cdb, &sqlite);

	sqlite3_prepare_v2(sqlite, "SELECT link FROM posts ORDER BY DATE;", -1, &stmt, NULL);

	while (sqlite3_step(stmt) == SQLITE_ROW)
		printf("%s\n", sqlite3_column_text(stmt, 0));

	sqlite3_finalize(stmt);
	sqlite3_close(sqlite);
	sqlite3_shutdown();
}

int
trimcr(char *str)
{
	size_t len;

	if (str == NULL)
		return -1;

	len = strlen(str);
	while (len--)
		if ((str[len] == '\r') || (str[len] == '\n'))
			str[len] = '\0';

	return 0;
}

static void
print_stmt(sqlite3_stmt *stmt)
{
	int icol;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		for (icol = 0; icol < sqlite3_column_count(stmt); icol++) {
			switch (sqlite3_column_type(stmt, icol)) {
				case SQLITE_TEXT:
					printf("%s: %s\n", sqlite3_column_name(stmt, icol), sqlite3_column_text(stmt, icol));
					break;
				case SQLITE_INTEGER:
					printf("%s: %lld\n", sqlite3_column_name(stmt, icol), sqlite3_column_int64(stmt, icol));
					break;
				default:
					/* do nothing */
					break;
			}
		}
	}
}

void
cblogctl_info(const char *post_name)
{
	sqlite3 *sqlite;
	sqlite3_stmt *stmt;

	sqlite3_initialize();
	sqlite3_open(cblog_cdb, &sqlite);

	if (sqlite3_prepare_v2(sqlite, "SELECT title, datetime(posts.date, 'unixepoch') as date "
	    "FROM posts "
	    "WHERE link=?1;", -1, &stmt, NULL) != SQLITE_OK)
		errx(1, "%s", sqlite3_errmsg(sqlite));

	sqlite3_bind_text(stmt, 1, post_name, -1, SQLITE_STATIC);
	print_stmt(stmt);
	sqlite3_finalize(stmt);
	if (sqlite3_prepare_v2(sqlite,
	    "SELECT group_concat(tag, ', ') as tags "
	    "FROM tags, tags_posts, posts WHERE link=?1 and posts.id=tags_posts.post_id and tags_posts.tag_id=tags.id;",
	    -1 , &stmt, NULL) != SQLITE_OK)
		errx(1, "%s", sqlite3_errmsg(sqlite));
	sqlite3_bind_text(stmt, 1, post_name, -1, SQLITE_STATIC);
	print_stmt(stmt);
	sqlite3_finalize(stmt);
	if (sqlite3_prepare_v2(sqlite,
	    "SELECT count(comment) as 'Number of comments' FROM comments, posts where posts.id = post_id and link=?1;",
	    -1 , &stmt, NULL) != SQLITE_OK)
		errx(1, "%s", sqlite3_errmsg(sqlite));
	sqlite3_bind_text(stmt, 1, post_name, -1, SQLITE_STATIC);
	print_stmt(stmt);
	sqlite3_finalize(stmt);
	printf("\n");
	sqlite3_close(sqlite);
	sqlite3_shutdown();
}

void
cblogctl_get(const char *post_name)
{
	sqlite3 *sqlite;
	sqlite3_stmt *stmt;
	int icol;
	FILE *out;

	sqlite3_initialize();
	sqlite3_open(cblog_cdb, &sqlite);

	out = fopen(post_name, "w");

	if (sqlite3_prepare_v2(sqlite, "SELECT title as Title, "
	    "group_concat(tag, ', ') as Tags, source "
	    "FROM posts, tags_posts, tags "
	    "WHERE link=?1 and posts.id=tags_posts.post_id and tags_posts.tag_id=tags.id;", -1, &stmt, NULL) != SQLITE_OK)
		errx(1, "%s", sqlite3_errmsg(sqlite));

	sqlite3_bind_text(stmt, 1, post_name, -1, SQLITE_STATIC);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		for (icol = 0; icol < sqlite3_column_count(stmt); icol++) {
			switch (sqlite3_column_type(stmt, icol)) {
				case SQLITE_TEXT:
					if (strcmp(sqlite3_column_name(stmt, icol),"source") == 0)
							fprintf(out, "\n%s\n",  sqlite3_column_text(stmt, icol));
					else
						fprintf(out, "%s: %s\n", sqlite3_column_name(stmt, icol), sqlite3_column_text(stmt, icol));
					break;
				case SQLITE_INTEGER:
					fprintf(out, "%s: %lld\n", sqlite3_column_name(stmt, icol), sqlite3_column_int64(stmt, icol));
					break;
				default:
					/* do nothing */
					break;
			}
		}
	}
	sqlite3_finalize(stmt);
	sqlite3_close(sqlite);
	sqlite3_shutdown();
	fclose(out);
}

void
cblogctl_add(const char *post_path)
{
	sqlite3 *sqlite;
	sqlite3_stmt *stmt, *stmtu;;
	FILE *post;
	char *ppath, *val;
	struct buf *ib, *ob;
	char filebuf[LINE_MAX];
	bool headers = true;

	ppath = basename(post_path);

	sqlite3_initialize();
	sqlite3_open(cblog_cdb, &sqlite);

	post = fopen(post_path, "r");

	if (post == NULL)
		errx(EXIT_FAILURE, "Unable to open %s", post_path);

	if (sqlite3_prepare_v2(sqlite, "INSERT INTO posts (link, title, source, html, date) "
	    "values (?1, trim(?2), ?3, ?4, strftime('%s', 'now'));",
	    -1, &stmt, NULL) != SQLITE_OK)
		errx(1, "%s", sqlite3_errmsg(sqlite));
	if (sqlite3_prepare_v2(sqlite, "UPDATE posts set title=?2, source=?3, html=?4 where link=?1;",
	    -1, &stmtu, NULL) != SQLITE_OK)
		errx(1, "%s", sqlite3_errmsg(sqlite));

	sqlite3_bind_text(stmt, 1, ppath, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmtu, 1, ppath, -1, SQLITE_STATIC);
	ib = bufnew(BUFSIZ);

	while (fgets(filebuf, LINE_MAX, post) != NULL) {
		if (filebuf[0] == '\n' && headers) {
			headers = false;
			continue;
		}

		if (headers) {
			trimcr(filebuf);
			if (STARTS_WITH(filebuf, "Title: ")) {
				val = filebuf + strlen("Title: ");
				sqlite3_bind_text(stmt, 2, val, -1, SQLITE_TRANSIENT);
				sqlite3_bind_text(stmtu, 2, val, -1, SQLITE_TRANSIENT);

			} else if (STARTS_WITH(filebuf, "Tags")) {
/*				while (isspace(filebuf[strlen(filebuf) - 1]))
					filebuf[strlen(filebuf) - 1] = '\0';

				val = filebuf + strlen("Tags: ");
				snprintf(key, BUFSIZ, "%s_tags", post_name);
				cdb_make_put(&cdb_make, key, strlen(key), val, strlen(val), CDB_PUT_REPLACE);*/
			}
		} else
			bufputs(ib, filebuf);
	}
	fclose(post);

	ob = bufnew(BUFSIZ);
	markdown(ob, ib, &mkd_xhtml);
	bufnullterm(ob);
	bufnullterm(ib);

	sqlite3_bind_text(stmt, 3, ib->data, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmtu, 3, ib->data, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 4, ob->data, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmtu, 4, ob->data, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		if (sqlite3_step(stmtu) != SQLITE_DONE)
			errx(1, "%s", sqlite3_errmsg(sqlite));
	}

	bufrelease(ib);
	bufrelease(ob);
}

void
cblogctl_del(const char *post_name)
{
	sqlite3 *sqlite;
	sqlite3_stmt *stmt;

	sqlite3_initialize();
	sqlite3_open(cblog_cdb, &sqlite);
	sql_exec(sqlite, "PRAGMA foreign_key=on;");

	if (sqlite3_prepare_v2(sqlite, "DELETE FROM posts WHERE link=?1;", -1, &stmt, NULL) != SQLITE_OK)
		errx(1, "%s", sqlite3_errmsg(sqlite));

	sqlite3_bind_text(stmt, 1, post_name, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	sql_exec(sqlite, "DELETE from tags where id not in select tag_id from tags_posts;");
	sqlite3_close(sqlite);
	sqlite3_shutdown();
}

void
cblogctl_set(const char *post_name, char *to_be_set)
{
	int					olddb, db, i;
	char				key[BUFSIZ];
	char				*val, *newkey, *valkey;
	struct cdb			cdb;
	struct cdb_make		cdb_make;
	struct cdb_find		cdbf;
	bool				found = false;

	if ((olddb = open(cblog_cdb, O_RDONLY)) < 0)
		err(1, "%s", cblog_cdb);
	if ((db = open(cblog_cdb_tmp, O_CREAT|O_RDWR|O_TRUNC, 0644)) < 0)
		err(1, "%s", cblog_cdb);

	cdb_init(&cdb, olddb);
	cdb_make_start(&cdb_make, db);

	cdb_findinit(&cdbf, &cdb, "posts", 5);
	while (cdb_findnext(&cdbf) > 0) {
		valkey = db_get(&cdb);
		cdb_make_add(&cdb_make, "posts", 5, valkey, strlen(valkey));

		if (EQUALS(post_name, valkey))
			found = true;

		for (i=0; field[i] != NULL; i++) {
			snprintf(key, BUFSIZ, "%s_%s", valkey, field[i]);
			if (cdb_find(&cdb, key, strlen(key)) > 0) {
				val = db_get(&cdb);
				cdb_make_add(&cdb_make, key, strlen(key), val, strlen(val));
				free(val);
			}
		}
		free(valkey);
	}
	if (!found)
		errx(EXIT_FAILURE, "%s: No such post", post_name);

	newkey = to_be_set;
	while (to_be_set[0] != '=')
		to_be_set++;

	to_be_set[0] = '\0';
	to_be_set++;

	snprintf(key, BUFSIZ, "%s_%s", post_name, newkey);

	cdb_make_put(&cdb_make, key, strlen(key), to_be_set, strlen(to_be_set), CDB_PUT_REPLACE);

	cdb_make_finish(&cdb_make);
	cdb_free(&cdb);
	close(db);
	close(olddb);

	if (rename(cblog_cdb_tmp, cblog_cdb) < 0)
		err(1, "%s", cblog_cdb);
}

void
cblogctl_create(void)
{
	int					db;
	struct cdb_make		cdb_make;

	if (access(cblog_cdb, F_OK) == 0)
		errx(1, "%s already exists", cblog_cdb);
	if ((db = open(cblog_cdb, O_CREAT|O_RDWR|O_TRUNC, 0644)) < 0)
		err(1, "%s", cblog_cdb);

	cdb_make_start(&cdb_make, db);
	cdb_make_finish(&cdb_make);
	close(db);
}

void
cblogctl_path(void)
{
	printf("%s\n", cblog_cdb);
}

void
cblogctl_version(void)
{
	fprintf(stderr, "%s (%s)\n", cblog_version, cblog_url);
}
/* vim: set sw=4 sts=4 ts=4 : */
