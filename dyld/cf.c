/*
 * A small part of CoreFoundation (CFString) for Mach-O programs that use it only to convert file names (7-Zip does).
 * A CFString here is a heap block with UTF-16 text; the program only handles it through these functions.
 * CFStringNormalize does nothing yet (names stay as they are: no Unicode tables).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "machrun.h"

typedef const void *CFTypeRef;
typedef long CFIndex;
typedef unsigned char Boolean;
typedef uint32_t CFStringEncoding;

#define	kCFStringEncodingASCII	0x0600
#define	kCFStringEncodingUTF8	0x08000100
#define	CF_MAGIC		0x43465354	/* "CFST" */

struct cfstr { uint32_t magic; int refcnt; CFIndex len; uint16_t *u; };

static struct cfstr *
cfnew(CFIndex len)
{
	struct cfstr *s = calloc(1, sizeof(*s));
	s->magic = CF_MAGIC; s->refcnt = 1; s->len = len; s->u = calloc(len + 1, sizeof(uint16_t));
	return (s);
}

/* UTF-8 -> UTF-16; bytes that are not valid UTF-8 become U+FFFD */
static CFIndex
utf8to16(const unsigned char *p, size_t n, uint16_t *out)
{
	CFIndex k = 0; size_t i = 0;
	while (i < n) {
		uint32_t c = p[i]; int more = 0;
		if (c < 0x80) { i++; }
		else if ((c & 0xE0) == 0xC0) { c &= 0x1F; more = 1; }
		else if ((c & 0xF0) == 0xE0) { c &= 0x0F; more = 2; }
		else if ((c & 0xF8) == 0xF0) { c &= 0x07; more = 3; }
		else { c = 0xFFFD; i++; goto emit; }
		if (more) {
			int j; i++;
			for (j = 0; j < more; j++, i++) { if (i >= n || (p[i] & 0xC0) != 0x80) { c = 0xFFFD; break; } c = (c << 6) | (p[i] & 0x3F); }
			if (j < more) { c = 0xFFFD; }
		}
emit:
		if (c >= 0x10000) { if (out) { out[k] = 0xD800 + ((c - 0x10000) >> 10); out[k + 1] = 0xDC00 + ((c - 0x10000) & 0x3FF); } k += 2; }
		else { if (out) out[k] = (uint16_t)c; k++; }
	}
	return (k);
}

void *
cf_CFStringCreateWithCString(void *alloc, const char *c, CFStringEncoding enc)
{
	size_t n; struct cfstr *s;
	if (c == NULL) return (NULL);
	n = strlen(c);
	if (enc == kCFStringEncodingUTF8) {
		s = cfnew(utf8to16((const unsigned char *)c, n, NULL));
		utf8to16((const unsigned char *)c, n, s->u);
	} else {					/* ASCII, Latin-1 and the rest: byte = code point */
		s = cfnew((CFIndex)n);
		for (size_t i = 0; i < n; i++) s->u[i] = (unsigned char)c[i];
	}
	return (s);
}
void *
cf_CFStringCreateWithCharacters(void *alloc, const uint16_t *chars, CFIndex n)
{
	struct cfstr *s = cfnew(n);
	memcpy(s->u, chars, n * sizeof(uint16_t));
	return (s);
}
CFIndex cf_CFStringGetLength(const struct cfstr *s) { return (s ? s->len : 0); }
uint16_t cf_CFStringGetCharacterAtIndex(const struct cfstr *s, CFIndex i) { return (s && i >= 0 && i < s->len ? s->u[i] : 0); }
void *
cf_CFStringCreateMutableCopy(void *alloc, CFIndex maxlen, const struct cfstr *src)
{
	return (cf_CFStringCreateWithCharacters(alloc, src->u, src->len));
}
Boolean
cf_CFStringGetCString(const struct cfstr *s, char *buf, CFIndex size, CFStringEncoding enc)
{
	CFIndex i, k = 0;
	for (i = 0; i < s->len; i++) {
		uint32_t c = s->u[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len && s->u[i + 1] >= 0xDC00 && s->u[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s->u[i + 1] - 0xDC00); i++; }
		char t[4]; int n;
		if (enc != kCFStringEncodingUTF8) { if (c > 0xFF) return (0); t[0] = (char)c; n = 1; }
		else if (c < 0x80) { t[0] = (char)c; n = 1; }
		else if (c < 0x800) { t[0] = 0xC0 | (c >> 6); t[1] = 0x80 | (c & 0x3F); n = 2; }
		else if (c < 0x10000) { char u[3] = { (char)(0xE0 | (c >> 12)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F)) }; memcpy(t, u, 3); n = 3; }
		else { char u[4] = { (char)(0xF0 | (c >> 18)), (char)(0x80 | ((c >> 12) & 0x3F)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F)) }; memcpy(t, u, 4); n = 4; }
		if (k + n + 1 > size) return (0);
		memcpy(buf + k, t, n); k += n;
	}
	if (k + 1 > size) return (0);
	buf[k] = 0;
	return (1);
}
void cf_CFStringNormalize(struct cfstr *s, int form) { (void)s; (void)form; }
CFTypeRef cf_CFRetain(struct cfstr *s) { if (s) s->refcnt++; return (s); }
void
cf_CFRelease(struct cfstr *s)
{
	if (s && s->magic == CF_MAGIC && --s->refcnt == 0) { s->magic = 0; free(s->u); free(s); }
}
