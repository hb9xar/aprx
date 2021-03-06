/* **************************************************************** *
 *                                                                  *
 *  APRX -- 2nd generation APRS iGate and digi with                 *
 *          minimal requirement of esoteric facilities or           *
 *          libraries of any kind beyond UNIX system libc.          *
 *                                                                  *
 * (c) Matti Aarnio - OH2MQK,  2007-2014                            *
 *                                                                  *
 * **************************************************************** */


#include "aprx.h"


/* aprxpolls libary functions.. */


void aprxpolls_reset(struct aprxpolls *app)
{
	app->pollcount = 0;
}

int aprxpolls_millis(struct aprxpolls *app)
{
	return tv_timerdelta_millis(&tick,&app->next_timeout);
}

struct pollfd *aprxpolls_new(struct aprxpolls *app)
{
	struct pollfd *p;
	app->pollcount += 1;
	if (app->pollcount >= app->pollsize) {
		app->pollsize += 8;
		app->polls = realloc(app->polls,
				     sizeof(struct pollfd) * app->pollsize);
		// valgrind polishing..
		p = &(app->polls[app->pollcount - 1]);
		memset(p, 0, sizeof(struct pollfd) * 8);
	}
	
        assert(app->polls);

	p = &(app->polls[app->pollcount - 1]);
	memset(p, 0, sizeof(struct pollfd));
	return p;
}

void aprxpolls_free(struct aprxpolls *app) {
	free(app->polls);
	app->polls = NULL;
}
