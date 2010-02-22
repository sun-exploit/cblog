#include <fcntl.h>
#include <locale.h>
#include <unistd.h>
#include <cdb.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>

#include "cblog_utils.h"
#include "cblog_common.h"
#include "cblog_cgi.h"

extern int errno;

static struct pages {
	const char	*name;
	int			type;
} page[] = {
	{ "/post", CBLOG_POST },
	{ "/tag", CBLOG_TAG },
	{ "/index.rss", CBLOG_RSS },
	{ "/index.atom", CBLOG_ATOM },
	{ NULL, -1 },
};

struct posts {
	char	*name;
	time_t	ctime;
};

struct tags {
	char	*name;
	int		count;
};

struct criteria {
	int		type;
	char	*tagname;
	time_t	start;
	time_t	end;
};

static int
sort_by_name(const void *a, const void *b)
{
	struct tags *ta = *((struct tags **)a);
	struct tags *tb = *((struct tags **)b);

	return strcasecmp(ta->name, tb->name);
}


static int
sort_by_ctime(const void *a, const void *b)
{
	struct posts *post_a = *((struct posts **)a);
	struct posts *post_b = *((struct posts **)b);

	return post_b->ctime - post_a->ctime;
}

static char *
trimspace(char *str)
{
	char *line = str;

	/* remove spaces at the beginning */
	while (true) {
		if (isspace(line[0]))
			line++;
		else
			break;
	}
	/* remove spaces at the end */
	while (true) {
		if (isspace(line[strlen(line) - 1]))
			line[strlen(line) - 1] = '\0';
		else
			break;
	}
	return line;
}

void
cblog_err(int eval, const char * message, ...)
{
	va_list		args;

	va_start(args, message);
	vsyslog(LOG_ERR, message, args);
}

static int
db_open(HDF *hdf, struct cdb *cdb, int flags)
{
	int fd, db;

	fd = db = open(get_cblog_db(hdf), flags);

	if (fd >= 0 && (db = cdb_init(cdb, fd)) < 0)
		close(fd);

	if (db < 0) {
		cblog_err(-1, "%s: %s", get_cblog_db(hdf), strerror(errno));
		hdf_set_value(hdf, "err_msg", strerror(errno));
	}

	return db;
}

void
add_post_to_hdf(HDF *hdf, struct cdb *cdb, char *name, int pos)
{
	int		i, j;
	char	key[BUFSIZ];
	char	*val;

	hdf_set_valuef(hdf, "Posts.%i.filename=%s", pos, name);
	for (i=0; field[i] != NULL; i++) {
		char *val_to_free;

		snprintf(key, BUFSIZ, "%s_%s", name, field[i]);
		cdb_find(cdb, key, strlen(key));
		val = db_get(cdb);
		val_to_free = val;

		if (EQUALS(field[i], "tags")) {
			int nbel = splitchr(val, ',');

			for (j=0; j <= nbel; j++) {
				size_t	next = strlen(val);
				char	*valtrimed = trimspace(val);

				hdf_set_valuef(hdf, "Posts.%i.tags.%i.name=%s", pos, j, valtrimed);
				val += next + 1;
			}
		} else if (EQUALS(field[i], "ctime")) {
			set_post_date(hdf, pos, val);
		} else
			hdf_set_valuef(hdf, "Posts.%i.%s=%s", pos, field[i], val);

		free(val_to_free);
	}
	hdf_set_valuef(hdf, "Posts.%i.nb_comments=%i", pos, get_comments_count(name));
}

void
set_tags(HDF *hdf)
{
	int					i, j, nbtags = 0;
	struct cdb			cdb;
	struct cdb_find		cdbf;
	char				key[BUFSIZ];
	char				*val;
	bool				found;
	struct tags			**taglist = NULL;

	if (db_open(hdf, &cdb, O_RDONLY) < 0)
		return;

	cdb_findinit(&cdbf, &cdb, "posts", 5);
	while (cdb_findnext(&cdbf) > 0) {
		int		nbel;
		char	*val_to_free;

		val = db_get(&cdb);
		snprintf(key, BUFSIZ, "%s_tags", val);
		free(val);

		cdb_find(&cdb, key, strlen(key));
		val = db_get(&cdb);

		val_to_free = val;
		nbel = splitchr(val, ',');
		for (i=0; i <= nbel; i++) {
			size_t	next = strlen(val);
			char	*tagcmp = trimspace(val);

			found = false;

			for (j=0; j < nbtags; j++) {
				if (EQUALS(tagcmp, taglist[j]->name)) {
					found = true;
					taglist[j]->count++;
					break;
				}
			}
			if (!found) {
				struct tags		*tag;

				nbtags++;
				taglist = realloc(taglist, nbtags * sizeof(struct tags*));
				tag = malloc(sizeof(struct tags));
				tag->name = strdup(tagcmp);

				tag->count = 1;
				taglist[nbtags - 1] = tag;
			}
			val+= next + 1;
		}
		free(val_to_free);
	}

	qsort(taglist, nbtags, sizeof(struct tags *), sort_by_name);

	for (i=0; i<nbtags; i++) {
		set_tag_name(hdf, i, taglist[i]->name);
		set_tag_count(hdf, i, taglist[i]->count);
		free(taglist[i]->name);
		free(taglist[i]);
	}
	free(taglist);

	close(cdb_fileno(&cdb));
	cdb_free(&cdb);
}

int
build_post(HDF *hdf, char *postname)
{
	int			ret = 0;
	struct cdb	cdb;
	char		key[BUFSIZ];
	char		*submit;

	submit = get_query_str(hdf, "submit");
	if (submit != NULL && EQUALS(submit, "Post"))
			set_comment(hdf, postname);

	if (db_open(hdf, &cdb, O_RDONLY) < 0)
		return 0;

	snprintf(key, BUFSIZ, "%s_title", postname);

	if (cdb_find(&cdb, key, strlen(key)) > 0) {
		add_post_to_hdf(hdf, &cdb, postname, 0);
		ret++;
	}

	get_comments(hdf, postname);

	close(cdb_fileno(&cdb));
	cdb_free(&cdb);
	return 1;
}

int
build_index(HDF *hdf, struct criteria *criteria)
{
	int					first_post = 0, nb_posts = 0, max_post, total_posts = 0;
	int					nb_pages = 0, page;
	int					i, j = 0, k;
	struct cdb			cdb;
	struct cdb_find		cdbf;
	char				*val;
	char				key[BUFSIZ];
	struct posts		**posts = NULL;

	max_post = hdf_get_int_value(hdf, "posts_per_pages", DEFAULT_POSTS_PER_PAGES);
	page = hdf_get_int_value(hdf, "Query.page", 1);
	first_post = (page * max_post) - max_post;

	if (db_open(hdf, &cdb, O_RDONLY) < 0)
		return 0;

	cdb_findinit(&cdbf, &cdb, "posts", 5);

	while (cdb_findnext(&cdbf) > 0) {
		struct posts *post = NULL;

		total_posts++;

		posts = realloc(posts, total_posts * sizeof(struct posts *));
		post = malloc(sizeof(struct posts));

		/* fetch the post key name */
		post->name = db_get(&cdb);

		snprintf(key, BUFSIZ, "%s_ctime", post->name);
		if (cdb_find(&cdb, key, strlen(key)) > 0){
			val = db_get(&cdb);
			post->ctime = (time_t)strtol(val, NULL, 10);
			free(val);
		} else
			post->ctime = time(NULL);

		posts[total_posts - 1] = post;
	}

	qsort(posts, total_posts, sizeof(struct posts *), sort_by_ctime);

	switch (criteria->type) {
		case CRITERIA_TAGNAME:
			for (i=0; i < total_posts; i++) {
				snprintf(key, BUFSIZ, "%s_tags", posts[i]->name);

				if (cdb_find(&cdb, key, strlen(key)) > 0) {
					char	*tag;
					int		nbel;

					val = db_get(&cdb);
					tag = val;
					nbel = splitchr(val, ',');
					for (k=0; k <= nbel; k++) {
						size_t	next = strlen(val);
						char	*tagcmp = trimspace(val);

						if (EQUALS(criteria->tagname, tagcmp)) {
							j++;
							if ((j >= first_post) && (nb_posts < max_post)) {
								add_post_to_hdf(hdf, &cdb, posts[i]->name, j);
								nb_posts++;
								break;
							}
						}
						val += next + 1;
					}
					free(tag);
				}
				free(posts[i]->name);
				free(posts[i]);
			}
			break;
		case CRITERIA_TIME_T:
			for (i=0; i < total_posts; i++) {
				if (posts[i]->ctime >= criteria->start && posts[i]->ctime <= criteria->end) {
					add_post_to_hdf(hdf, &cdb, posts[i]->name, i);
					nb_posts++;
				}
				free(posts[i]->name);
				free(posts[i]);
			}
			break;
		default:
			for (i=first_post; i < total_posts; i++) {
				if ((i >= first_post) && (nb_posts < max_post)) {
					add_post_to_hdf(hdf, &cdb, posts[i]->name, i);
					nb_posts++;
				}
				free(posts[i]->name);
				free(posts[i]);
			}
	}

	free(posts);

	nb_pages = j / max_post;
	if (j % max_post > 0)
		nb_pages++;

	set_nb_pages(hdf, nb_pages);

	close(cdb_fileno(&cdb));
	cdb_free(&cdb);

	return nb_posts;
}

void
cblogcgi(HDF *conf)
{
	CGI		*cgi;
	NEOERR	*neoerr;
	HDF		*hdf;
	STRING	neoerr_str;
	char	*requesturi, *method;
	int		type, i, nb_posts;
	time_t		gentime, posttime;
	int     yyyy, mm, dd;
	struct	criteria criteria;
	struct tm calc_time;

	/* read the configuration file */

	type = CBLOG_ROOT;
	criteria.type = 0;

	neoerr = cgi_init(&cgi, NULL);

	nerr_ignore(&neoerr);

	method = get_cgi_str(cgi->hdf, "RequestMethod");

	neoerr = cgi_parse(cgi);
	nerr_ignore(&neoerr);

	neoerr = hdf_copy(cgi->hdf, "", conf);
	nerr_ignore(&neoerr);
	hdf_set_valuef(cgi->hdf, "CBlog.version=%s", cblog_version);
	hdf_set_valuef(cgi->hdf, "CBlog.url=%s", cblog_url);

	requesturi = get_cgi_str(cgi->hdf, "RequestURI");

	/* find the beginig of the request */
	if (requesturi != NULL)
	{
		splitchr(requesturi, '?');
		if (requesturi[1] != '\0') {
			for (i = 0; page[i].name != NULL; i++) {
				if (STARTS_WITH(requesturi, page[i].name)) {
					type = page[i].type;
					break;
				}
			}
			if (type == CBLOG_ROOT) {
				if (sscanf(requesturi, "/%4d/%02d/%02d", &yyyy, &mm, &dd) == 3) {
					type = CBLOG_YYYY_MM_DD;
					criteria.type = CRITERIA_TIME_T;
				} else if (sscanf(requesturi, "/%4d/%02d", &yyyy, &mm) == 2) {
					type = CBLOG_YYYY_MM;
					criteria.type = CRITERIA_TIME_T;
				} else if (sscanf(requesturi, "/%4d", &yyyy) == 1) {
					type = CBLOG_YYYY;
					criteria.type = CRITERIA_TIME_T;
				} else {
					hdf_set_valuef(cgi->hdf, "err_msg=Unknown request: %s", requesturi);
				}
			}
		}
	}

	switch (type) {
		case CBLOG_POST:
			requesturi++;
			while (requesturi[0] != '/')
				requesturi++;
			requesturi++;
			nb_posts = build_post(cgi->hdf, requesturi);
			if (nb_posts == 0) {
				hdf_set_valuef(cgi->hdf, "err_msg=Unknown post: %s", requesturi);
				cgiwrap_writef("Status: 404\n");
			}
			break;
		case CBLOG_TAG:
			requesturi++;
			while (requesturi[0] != '/')
				requesturi++;
			requesturi++;
			criteria.type = CRITERIA_TAGNAME;
			criteria.tagname = requesturi;
			nb_posts = build_index(cgi->hdf, &criteria);
			if (nb_posts == 0) {
				hdf_set_valuef(cgi->hdf, "err_msg=Unknown tag: %s", requesturi);
				cgiwrap_writef("Status: 404\n");
			}
			break;
		case CBLOG_RSS:
			hdf_set_valuef(cgi->hdf, "Query.feed=rss");
			build_index(cgi->hdf, &criteria);
			break;
		case CBLOG_ATOM:
			hdf_set_valuef(cgi->hdf, "Query.feed=atom");
			build_index(cgi->hdf, &criteria);
			break;
		case CBLOG_ROOT:
			build_index(cgi->hdf, &criteria);
			break;
		case CBLOG_YYYY:
			calc_time.tm_year = yyyy - 1900;
			calc_time.tm_mon = 0;
			calc_time.tm_mday = 1;
			criteria.start = mktime(&calc_time);
			calc_time.tm_mon = 11;
			calc_time.tm_mday = 31;
			calc_time.tm_hour = 23;
			calc_time.tm_min = 59;
			calc_time.tm_sec = 59;
			criteria.end = mktime(&calc_time);
			build_index(cgi->hdf, &criteria);
			break;
		case CBLOG_YYYY_MM:
			calc_time.tm_year = yyyy - 1900;
			calc_time.tm_mon = mm - 1;
			calc_time.tm_mday = 1;
			criteria.start = mktime(&calc_time);
			calc_time.tm_mon = mm;
			calc_time.tm_hour = 23;
			calc_time.tm_min = 59;
			calc_time.tm_sec = 59;
			criteria.end = mktime(&calc_time);
			criteria.end -= 60 * 60 * 24;
			build_index(cgi->hdf, &criteria);
			break;
		case CBLOG_YYYY_MM_DD:
			calc_time.tm_year = yyyy - 1900;
			calc_time.tm_mon = mm - 1;
			calc_time.tm_mday = dd;
			criteria.start = mktime(&calc_time);
			calc_time.tm_hour = 23;
			calc_time.tm_min = 59;
			calc_time.tm_sec = 59;
			criteria.end = mktime(&calc_time);
			build_index(cgi->hdf, &criteria);
			break;
	}

	if (hdf_get_value(cgi->hdf, "err_msg", NULL) == NULL) {
		if (EQUALS(hdf_get_value(cgi->hdf, "Query.feed", "fail"), "rss")) {
			char	time_str[31];

			setlocale(LC_ALL, "C");
			HDF_FOREACH(hdf, cgi->hdf, "Posts") {
				int		date = hdf_get_int_value(hdf, "date", time(NULL));

				time_to_str(date, DATE_FEED, time_str, 31);

				hdf_set_valuef(hdf, "date=%s", time_str);
			}

			gentime = time(NULL);
			time_to_str(gentime, DATE_FEED, time_str, 31);

			hdf_set_valuef(cgi->hdf, "gendate=%s", time_str);

			hdf_set_valuef(cgi->hdf, "cgiout.ContentType=application/rss+xml");
			neoerr = cgi_display(cgi, hdf_get_value(cgi->hdf, "feed.rss", "rss.cs"));
		} else if (EQUALS(hdf_get_value(cgi->hdf, "Query.feed", "fail"), "atom")) {
			struct tm	*date;
			HDF			*hdf;

			HDF_FOREACH(hdf, cgi->hdf, "Posts") {

				posttime = hdf_get_int_value(hdf, "date", time(NULL));
				date = gmtime(&posttime);

				hdf_set_valuef(hdf, "date=%04d-%02d-%02dT%02d:%02d:%02dZ",
						date->tm_year + 1900, date->tm_mon + 1, date->tm_mday, 
						date->tm_hour, date->tm_min, date->tm_sec);
			}

			gentime = time(NULL);
			date = gmtime(&gentime);

			hdf_set_valuef(cgi->hdf, "gendate=%04d-%02d-%02dT%02d:%02d:%02dZ",
					date->tm_year + 1900, date->tm_mon + 1, date->tm_mday, 
					date->tm_hour, date->tm_min, date->tm_sec);

			hdf_set_valuef(cgi->hdf, "cgiout.ContentType=application/atom+xml");
			neoerr = cgi_display(cgi, hdf_get_value(cgi->hdf, "feed.atom", "atom.cs"));
		} else {
			char	time_str[256];
			char	*date_format;
			int		date;
			
			date_format = get_dateformat(cgi->hdf);
			set_tags(cgi->hdf);

			HDF_FOREACH(hdf, cgi->hdf, "Posts") {
				date = hdf_get_int_value(hdf, "date", time(NULL));
				time_to_str(date, date_format, time_str, 256);

				hdf_set_valuef(hdf, "date=%s", time_str);
			}
			neoerr = cgi_display(cgi, get_cgi_theme(cgi->hdf));
		}
	} else {
		cgiwrap_writef("Status: 404\n");
		set_tags(cgi->hdf);
		neoerr = cgi_display(cgi, get_cgi_theme(cgi->hdf));
	}

	if (neoerr != STATUS_OK && strcmp(method, "HEAD") != 0 ) {
		nerr_error_string(neoerr, &neoerr_str);
		cblog_err(-1, neoerr_str.buf);
		string_clear(&neoerr_str);
	}
	nerr_ignore(&neoerr);
	cgi_destroy(&cgi);
}

/* vim: set sw=4 sts=4 ts=4 : */
