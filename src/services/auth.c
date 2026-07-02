#include "services/auth.h"

#include "core/log.h"

#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* PAM services to try, in order. A screen locker normally ships /etc/pam.d/
 * <name>; absent that we fall back to common existing services. */
static const char *const PAM_SERVICES[] = {"dankc", "swaylock", "login", "system-auth"};

/* Supply the password for PAM_PROMPT_ECHO_OFF prompts. appdata = password. */
static int conv_fn(int num_msg, const struct pam_message **msg, struct pam_response **resp,
                   void *appdata)
{
    if (num_msg <= 0)
        return PAM_CONV_ERR;
    struct pam_response *r = calloc((size_t)num_msg, sizeof(*r));
    if (!r)
        return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; i++) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
            r[i].resp = strdup((const char *)appdata);
    }
    *resp = r;
    return PAM_SUCCESS;
}

static bool try_service(const char *service, const char *user, const char *password)
{
    struct pam_conv conv = {conv_fn, (void *)password};
    pam_handle_t *pamh = NULL;
    if (pam_start(service, user, &conv, &pamh) != PAM_SUCCESS)
        return false;
    int rc = pam_authenticate(pamh, 0);
    pam_end(pamh, rc);
    return rc == PAM_SUCCESS;
}

bool dc_auth_check(const char *password)
{
    if (!password)
        return false;
    const char *user = getenv("USER");
    if (!user || !*user) {
        struct passwd *pw = getpwuid(getuid());
        user = pw ? pw->pw_name : NULL;
    }
    if (!user)
        return false;

    for (size_t i = 0; i < sizeof(PAM_SERVICES) / sizeof(PAM_SERVICES[0]); i++) {
        if (try_service(PAM_SERVICES[i], user, password)) {
            dc_debug("auth: PAM service '%s' accepted", PAM_SERVICES[i]);
            return true;
        }
    }
    return false;
}
