#include "RSserver.h"

#define SOCK_ERRORS
#define LISTENQ 16

#include <sisocks.h>
#include <sys/un.h> /* needed for unix sockets */
#include <sys/types.h>
#include <sys/stat.h>

server_t *create_server(int port, const char *localSocketName, int localSocketMode) {
	server_t *srv;
	SAIN ssa;
	int reuse, ss;
	struct sockaddr_in lsa;
	struct sockaddr_un lusa;
    
	lsa.sin_addr.s_addr = inet_addr("127.0.0.1");
    
	initsocks();
	if (localSocketName) {
#ifndef unix
		fprintf(stderr,"Local sockets are not supported on non-unix systems.\n");
		return 0;
#else
		ss = FCF("open socket", socket(AF_LOCAL, SOCK_STREAM, 0));
		memset(&lusa, 0, sizeof(lusa));
		lusa.sun_family = AF_LOCAL;
		if (strlen(localSocketName) > sizeof(lusa.sun_path) - 2) {
			fprintf(stderr,"Local socket name is too long for this system.\n");
			return 0;
		}
		strcpy(lusa.sun_path, localSocketName);
		remove(localSocketName); /* remove existing if possible */
#endif
	} else
		ss = FCF("open socket", socket(AF_INET, SOCK_STREAM, 0));

	srv = (server_t*) calloc(1, sizeof(server_t));
	if (!srv) {
		fprintf(stderr, "ERROR: cannot allocate memory for server structure\n");
		return 0;
	}

	srv->ss = ss;
	srv->unix_socket = localSocketName ? 1 : 0;

	reuse = 1; /* enable socket address reusage */
	setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

#ifdef unix
	if (localSocketName) {
		FCF("bind", bind(ss, (SA*) &lusa, sizeof(lusa)));    
		if (localSocketMode)
			chmod(localSocketName, localSocketMode);
	} else
#endif
		FCF("bind", bind(ss, build_sin(&ssa, 0, port), sizeof(ssa)));
    
	FCF("listen", listen(ss, LISTENQ));

	return srv;
}

/*--- The following makes the indenting behavior of emacs compatible
      with Xcode's 4/4 setting ---*/
/* Local Variables: */
/* indent-tabs-mode: t */
/* tab-width: 4 */
/* c-basic-offset: 4 */
/* End: */