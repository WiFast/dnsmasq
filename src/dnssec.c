/* dnssec.c is Copyright (c) 2012 Giovanni Bajo <rasky@develer.com>
           and Copyright (c) 2012-2014 Simon Kelley

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 dated June, 1991, or
   (at your option) version 3 dated 29 June, 2007.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dnsmasq.h"

#ifdef HAVE_DNSSEC

#include <nettle/rsa.h>
#include <nettle/dsa.h>
#include <nettle/nettle-meta.h>
#include <gmp.h>

#define SERIAL_UNDEF  -100
#define SERIAL_EQ        0
#define SERIAL_LT       -1
#define SERIAL_GT        1

/* http://www.iana.org/assignments/ds-rr-types/ds-rr-types.xhtml */
static char *ds_digest_name(int digest)
{
  switch (digest)
    {
    case 1: return "sha1";
    case 2: return "sha256";
    case 3: return "gosthash94";
    case 4: return "sha384";
    default: return NULL;
    }
}
 
/* http://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml */
static char *algo_digest_name(int algo)
{
  switch (algo)
    {
    case 1: return "md5";
    case 3: return "sha1";
    case 5: return "sha1";
    case 6: return "sha1";
    case 7: return "sha1";
    case 8: return "sha256";
    case 10: return "sha512";
    case 12: return "gosthash94";
    case 13: return "sha256";
    case 14: return "sha384";
    default: return NULL;
    }
}
      
/* Find pointer to correct hash function in nettle library */
static const struct nettle_hash *hash_find(char *name)
{
  int i;
  
  if (!name)
    return NULL;
  
  for (i = 0; nettle_hashes[i]; i++)
    {
      if (strcmp(nettle_hashes[i]->name, name) == 0)
	return nettle_hashes[i];
    }

  return NULL;
}

/* expand ctx and digest memory allocations if necessary and init hash function */
static int hash_init(const struct nettle_hash *hash, void **ctxp, unsigned char **digestp)
{
  static void *ctx = NULL;
  static unsigned char *digest = NULL;
  static unsigned int ctx_sz = 0;
  static unsigned int digest_sz = 0;

  void *new;

  if (ctx_sz < hash->context_size)
    {
      if (!(new = whine_malloc(hash->context_size)))
	return 0;
      if (ctx)
	free(ctx);
      ctx = new;
      ctx_sz = hash->context_size;
    }
  
  if (digest_sz < hash->digest_size)
    {
      if (!(new = whine_malloc(hash->digest_size)))
	return 0;
      if (digest)
	free(digest);
      digest = new;
      digest_sz = hash->digest_size;
    }

  *ctxp = ctx;
  *digestp = digest;

  hash->init(ctx);

  return 1;
}
  
static int rsa_verify(struct blockdata *key_data, unsigned int key_len, unsigned char *sig, size_t sig_len,
		      unsigned char *digest, int algo)
{
  unsigned char *p;
  size_t exp_len;
  
  static struct rsa_public_key *key = NULL;
  static mpz_t sig_mpz;
  
  if (key == NULL)
    {
      if (!(key = whine_malloc(sizeof(struct rsa_public_key))))
	return 0;
      
      nettle_rsa_public_key_init(key);
      mpz_init(sig_mpz);
    }
  
  if ((key_len < 3) || !(p = blockdata_retrieve(key_data, key_len, NULL)))
    return 0;
  
  key_len--;
  if ((exp_len = *p++) == 0)
    {
      GETSHORT(exp_len, p);
      key_len -= 2;
    }
  
  if (exp_len >= key_len)
    return 0;
  
  key->size =  key_len - exp_len;
  mpz_import(key->e, exp_len, 1, 1, 0, 0, p);
  mpz_import(key->n, key->size, 1, 1, 0, 0, p + exp_len);

  mpz_import(sig_mpz, sig_len, 1, 1, 0, 0, sig);
  
  switch (algo)
    {
    case 1:
      return nettle_rsa_md5_verify_digest(key, digest, sig_mpz);
    case 5: case 7:
      return nettle_rsa_sha1_verify_digest(key, digest, sig_mpz);
    case 8:
      return nettle_rsa_sha256_verify_digest(key, digest, sig_mpz);
    case 10:
      return nettle_rsa_sha512_verify_digest(key, digest, sig_mpz);
    }

  return 0;
}  

static int dsa_verify(struct blockdata *key_data, unsigned int key_len, unsigned char *sig, size_t sig_len,
		      unsigned char *digest, int algo)
{
  unsigned char *p;
  unsigned int t;
  
  static struct dsa_public_key *key = NULL;
  static struct dsa_signature *sig_struct;
  
  if (key == NULL)
    {
      if (!(sig_struct = whine_malloc(sizeof(struct dsa_signature))) || 
	  !(key = whine_malloc(sizeof(struct dsa_public_key)))) 
	return 0;
      
      nettle_dsa_public_key_init(key);
      nettle_dsa_signature_init(sig_struct);
    }
  
  if ((sig_len < 41) || !(p = blockdata_retrieve(key_data, key_len, NULL)))
    return 0;
  
  t = *p++;
  
  if (key_len < (213 + (t * 24)))
    return 0;

  mpz_import(key->q, 20, 1, 1, 0, 0, p); p += 20;
  mpz_import(key->p, 64 + (t*8), 1, 1, 0, 0, p); p += 64 + (t*8);
  mpz_import(key->g, 64 + (t*8), 1, 1, 0, 0, p); p += 64 + (t*8);
  mpz_import(key->y, 64 + (t*8), 1, 1, 0, 0, p); p += 64 + (t*8);
  
  mpz_import(sig_struct->r, 20, 1, 1, 0, 0, sig+1);
  mpz_import(sig_struct->s, 20, 1, 1, 0, 0, sig+21);
  
  (void)algo;

  return nettle_dsa_sha1_verify_digest(key, digest, sig_struct);
} 
 
static int verify(struct blockdata *key_data, unsigned int key_len, unsigned char *sig, size_t sig_len,
		  unsigned char *digest, int algo)
{
  switch (algo)
    {
    case 1: case 5: case 7: case 8: case 10:
      return rsa_verify(key_data, key_len, sig, sig_len, digest, algo);
      
    case 3: case 6: 
      return dsa_verify(key_data, key_len, sig, sig_len, digest, algo);
    }
  
  return 0;
}

/* Convert from presentation format to wire format, in place.
   Also map UC -> LC.
   Note that using extract_name to get presentation format
   then calling to_wire() removes compression and maps case,
   thus generating names in canonical form.
   Calling to_wire followed by from_wire is almost an identity,
   except that the UC remains mapped to LC. 
*/
static int to_wire(char *name)
{
  unsigned char *l, *p, term;
  int len;

  for (l = (unsigned char*)name; *l != 0; l = p)
    {
      for (p = l; *p != '.' && *p != 0; p++)
	if (*p >= 'A' && *p <= 'Z')
	  *p = *p - 'A' + 'a';
      
      term = *p;
      
      if ((len = p - l) != 0)
	memmove(l+1, l, len);
      *l = len;
      
      p++;
      
      if (term == 0)
	*p = 0;
    }
  
  return l + 1 - (unsigned char *)name;
}

/* Note: no compression  allowed in input. */
static void from_wire(char *name)
{
  unsigned char *l;
  int len;

  for (l = (unsigned char *)name; *l != 0; l += len+1)
    {
      len = *l;
      memmove(l, l+1, len);
      l[len] = '.';
    }

  *(l-1) = 0;
}

/* Input in presentation format */
static int count_labels(char *name)
{
  int i;

  if (*name == 0)
    return 0;

  for (i = 0; *name; name++)
    if (*name == '.')
      i++;

  return i+1;
}

/* Implement RFC1982 wrapped compare for 32-bit numbers */
static int serial_compare_32(unsigned long s1, unsigned long s2)
{
  if (s1 == s2)
    return SERIAL_EQ;

  if ((s1 < s2 && (s2 - s1) < (1UL<<31)) ||
      (s1 > s2 && (s1 - s2) > (1UL<<31)))
    return SERIAL_LT;
  if ((s1 < s2 && (s2 - s1) > (1UL<<31)) ||
      (s1 > s2 && (s1 - s2) < (1UL<<31)))
    return SERIAL_GT;
  return SERIAL_UNDEF;
}

/* Check whether today/now is between date_start and date_end */
static int check_date_range(unsigned long date_start, unsigned long date_end)
{
  unsigned long curtime = time(0);
  
  /* We must explicitly check against wanted values, because of SERIAL_UNDEF */
  return serial_compare_32(curtime, date_start) == SERIAL_GT
    && serial_compare_32(curtime, date_end) == SERIAL_LT;
}

static u16 *get_desc(int type)
{
  /* List of RRtypes which include domains in the data.
     0 -> domain
     integer -> no of plain bytes
     -1 -> end

     zero is not a valid RRtype, so the final entry is returned for
     anything which needs no mangling.
  */
  
  static u16 rr_desc[] = 
    { 
      T_NS, 0, -1, 
      T_MD, 0, -1,
      T_MF, 0, -1,
      T_CNAME, 0, -1,
      T_SOA, 0, 0, -1,
      T_MB, 0, -1,
      T_MG, 0, -1,
      T_MR, 0, -1,
      T_PTR, 0, -1,
      T_MINFO, 0, 0, -1,
      T_MX, 2, 0, -1,
      T_RP, 0, 0, -1,
      T_AFSDB, 2, 0, -1,
      T_RT, 2, 0, -1,
      T_SIG, 18, 0, -1,
      T_PX, 2, 0, 0, -1,
      T_NXT, 0, -1,
      T_KX, 2, 0, -1,
      T_SRV, 6, 0, -1,
      T_DNAME, 0, -1,
      T_RRSIG, 18, 0, -1,
      T_NSEC, 0, -1,
      0, -1 /* wildcard/catchall */
    }; 
  
  u16 *p = rr_desc;
  
  while (*p != type && *p != 0)
    while (*p++ != (u16)-1);

  return p+1;
}

/* Return bytes of canonicalised rdata, when the return value is zero, the remaining 
   data, pointed to by *p, should be used raw. */
static int get_rdata(struct dns_header *header, size_t plen, unsigned char *end, char *buff, 
		     unsigned char **p, u16 **desc)
{
  int d = **desc;
  
  (*desc)++;
  
  /* No more data needs mangling */
  if (d == (u16)-1)
    return 0;
  
  if (d == 0 && extract_name(header, plen, p, buff, 1, 0))
    /* domain-name, canonicalise */
    return to_wire(buff);
  else
    { 
      /* plain data preceding a domain-name, don't run off the end of the data */
      if ((end - *p) < d)
	d = end - *p;
      
      if (d != 0)
	{
	  memcpy(buff, *p, d);
	  *p += d;
	}
      
      return d;
    }
}

/* Bubble sort the RRset into the canonical order. 
   Note that the byte-streams from two RRs may get unsynced: consider 
   RRs which have two domain-names at the start and then other data.
   The domain-names may have different lengths in each RR, but sort equal

   ------------
   |abcde|fghi|
   ------------
   |abcd|efghi|
   ------------

   leaving the following bytes as deciding the order. Hence the nasty left1 and left2 variables.
*/

static void sort_rrset(struct dns_header *header, size_t plen, u16 *rr_desc, int rrsetidx, 
		       unsigned char **rrset, char *buff1, char *buff2)
{
  int swap, quit, i;
  
  do
    {
      for (swap = 0, i = 0; i < rrsetidx-1; i++)
	{
	  int rdlen1, rdlen2, left1, left2, len1, len2, len, rc;
	  u16 *dp1, *dp2;
	  unsigned char *end1, *end2;
	  unsigned char *p1 = skip_name(rrset[i], header, plen, 10);
	  unsigned char *p2 = skip_name(rrset[i+1], header, plen, 10);
	  
	  p1 += 8; /* skip class, type, ttl */
	  GETSHORT(rdlen1, p1);
	  end1 = p1 + rdlen1;
	  
	  p2 += 8; /* skip class, type, ttl */
	  GETSHORT(rdlen2, p2);
	  end2 = p2 + rdlen2; 
	  
	  dp1 = dp2 = rr_desc;
	  
	  for (quit = 0, left1 = 0, left2 = 0, len1 = 0, len2 = 0; !quit;)
	    {
	      if (left1 != 0)
		memmove(buff1, buff1 + len1 - left1, left1);
	      
	      if ((len1 = get_rdata(header, plen, end1, buff1 + left1, &p1, &dp1)) == 0)
		{
		  quit = 1;
		  len1 = end1 - p1;
		  memcpy(buff1 + left1, p1, len1);
		}
	      len1 += left1;
	      
	      if (left2 != 0)
		memmove(buff2, buff2 + len2 - left2, left2);
	      
	      if ((len2 = get_rdata(header, plen, end2, buff2 + left2, &p2, &dp2)) == 0)
		{
		  quit = 1;
		  len2 = end2 - p2;
		  memcpy(buff2 + left2, p2, len2);
		}
	      len2 += left2;
	       
	      if (len1 > len2)
		left1 = len1 - len2, left2 = 0, len = len2;
	      else
		left2 = len2 - len1, left1 = 0, len = len1;
	      
	      rc = memcmp(buff1, buff2, len);
	      
	      if (rc > 0 || (rc == 0 && quit && len1 > len2))
		{
		  unsigned char *tmp = rrset[i+1];
		  rrset[i+1] = rrset[i];
		  rrset[i] = tmp;
		  swap = quit = 1;
		}
	    }
	}
    } while (swap);
}

/* Validate a single RRset (class, type, name) in the supplied DNS reply 
   Return code:
   STAT_SECURE   if it validates.
   STAT_INSECURE can't validate (no RRSIG, bad packet).
   STAT_BOGUS    signature is wrong.
   STAT_NEED_KEY need DNSKEY to complete validation (name is returned in keyname)

   if key is non-NULL, use that key, which has the algo and tag given in the params of those names,
   otherwise find the key in the cache.
*/
static int validate_rrset(time_t now, struct dns_header *header, size_t plen, int class, 
		   int type, char *name, char *keyname, struct blockdata *key, int keylen, int algo_in, int keytag_in)
{
  static unsigned char **rrset = NULL, **sigs = NULL;
  static int rrset_sz = 0, sig_sz = 0;
  
  unsigned char *p;
  int rrsetidx, sigidx, res, rdlen, j, name_labels;
  struct crec *crecp = NULL;
  int type_covered, algo, labels, orig_ttl, sig_expiration, sig_inception, key_tag;
  u16 *rr_desc = get_desc(type);

  if (!(p = skip_questions(header, plen)))
    return STAT_INSECURE;

  name_labels = count_labels(name); /* For 4035 5.3.2 check */

  /* look for RRSIGs for this RRset and get pointers to each RR in the set. */
  for (rrsetidx = 0, sigidx = 0, j = ntohs(header->ancount) + ntohs(header->nscount); 
       j != 0; j--) 
    {
      unsigned char *pstart, *pdata;
      int stype, sclass;

      pstart = p;
      
      if (!(res = extract_name(header, plen, &p, name, 0, 10)))
	return STAT_INSECURE; /* bad packet */
      
      GETSHORT(stype, p);
      GETSHORT(sclass, p);
      p += 4; /* TTL */
      
      pdata = p;

      GETSHORT(rdlen, p);
      
      if (res == 1 && sclass == class)
	{
	  if (stype == type)
	    {
	      if (rrsetidx == rrset_sz)
		{
		  unsigned char **new;

		  /* expand */
		  if (!(new = whine_malloc((rrset_sz + 5) * sizeof(unsigned char **))))
		    return STAT_INSECURE;
		  
		  if (rrset)
		    {
		      memcpy(new, rrset, rrset_sz * sizeof(unsigned char **));
		      free(rrset);
		    }
		  
		  rrset = new;
		  rrset_sz += 5;
		}
	      rrset[rrsetidx++] = pstart;
	    }
	  
	  if (stype == T_RRSIG)
	    {
	       if (rdlen < 18)
		 return STAT_INSECURE; /* bad packet */ 
	         
	       GETSHORT(type_covered, p);
	       algo = *p++;
	       labels = *p++;
	       p += 4; /* orig_ttl */
	       GETLONG(sig_expiration, p);
	       GETLONG(sig_inception, p);
	       p = pdata + 2; /* restore for ADD_RDLEN */
	       
	       if (type_covered == type &&
		   check_date_range(sig_inception, sig_expiration) &&
		   hash_find(algo_digest_name(algo)) &&
		   labels <= name_labels)
		 {
		   if (sigidx == sig_sz)
		     {
		       unsigned char **new;
		       
		       /* expand */
		       if (!(new = whine_malloc((sig_sz + 5) * sizeof(unsigned char **))))
			 return STAT_INSECURE;
		       
		       if (sigs)
			 {
			   memcpy(new, sigs, sig_sz * sizeof(unsigned char **));
			   free(sigs);
			 }
		       
		       sigs = new;
		       sig_sz += 5;
		     }
		   
		   sigs[sigidx++] = pdata;
		 }
	    }
	}
     
      if (!ADD_RDLEN(header, p, plen, rdlen))
	return STAT_INSECURE;
    }
  
  /* RRset empty, no RRSIGs */
  if (rrsetidx == 0 || sigidx == 0)
    return STAT_INSECURE; 
  
  /* Sort RRset records into canonical order. 
     Note that at this point keyname and name buffs are
     unused, and used as workspace by the sort. */
  sort_rrset(header, plen, rr_desc, rrsetidx, rrset, name, keyname);
         
  /* Now try all the sigs to try and find one which validates */
  for (j = 0; j <sigidx; j++)
    {
      unsigned char *psav, *sig;
      int i, wire_len, sig_len;
      const struct nettle_hash *hash;
      void *ctx;
      unsigned char *digest;
      u32 nsigttl;
      
      p = sigs[j];
      GETSHORT(rdlen, p); /* rdlen >= 18 checked previously */
      psav = p;
      
      p += 2; /* type_covered - already checked */
      algo = *p++;
      labels = *p++;
      GETLONG(orig_ttl, p);
      p += 8; /* sig_expiration and sig_inception */
      GETSHORT(key_tag, p);
      
      if (!extract_name(header, plen, &p, keyname, 1, 0))
	return STAT_INSECURE;
      
      /* OK, we have the signature record, see if the relevant DNSKEY is in the cache. */
      if (!key && !(crecp = cache_find_by_name(NULL, keyname, now, F_DNSKEY)))
	return STAT_NEED_KEY;
      
      sig = p;
      sig_len = rdlen - (p - psav);
       
      if (!(hash = hash_find(algo_digest_name(algo))) ||
	  !hash_init(hash, &ctx, &digest))
	continue;
       
      nsigttl = htonl(orig_ttl);
      
      hash->update(ctx, 18, psav);
      wire_len = to_wire(keyname);
      hash->update(ctx, (unsigned int)wire_len, (unsigned char*)keyname);
      from_wire(keyname);
      
      for (i = 0; i < rrsetidx; ++i)
	{
	  int seg;
	  unsigned char *end, *cp;
	  char *name_start = name;
	  u16 len, *dp;

	  p = rrset[i];
	  if (!extract_name(header, plen, &p, name, 1, 10)) 
	    return STAT_INSECURE;

	  /* if more labels than in RRsig name, hash *.<no labels in rrsig labels field>  4035 5.3.2 */
	  if (labels < name_labels)
	    {
	      int k;
	      for (k = name_labels - labels; k != 0; k--)
		while (*name_start != '.' && *name_start != 0)
		  name_start++;
	      name_start--;
	      *name_start = '*';
	    }
	  
	  wire_len = to_wire(name_start);
	  hash->update(ctx, (unsigned int)wire_len, (unsigned char *)name_start);
	  hash->update(ctx, 4, p); /* class and type */
	  hash->update(ctx, 4, (unsigned char *)&nsigttl);
	  
	  p += 8; /* skip class, type, ttl */
	  GETSHORT(rdlen, p);
	  if (!CHECK_LEN(header, p, plen, rdlen))
	    return STAT_INSECURE; 
	  
	  end = p + rdlen;
	  
	  /* canonicalise rdata and calculate length of same, use name buffer as workspace */
	  cp = p;
	  dp = rr_desc;
	  for (len = 0; (seg = get_rdata(header, plen, end, name, &cp, &dp)) != 0; len += seg);
	  len += end - cp;
	  len = htons(len);
	  hash->update(ctx, 2, (unsigned char *)&len); 
	  
	  /* Now canonicalise again and digest. */
	  cp = p;
	  dp = rr_desc;
	  while ((seg = get_rdata(header, plen, end, name, &cp, &dp)))
	    hash->update(ctx, seg, (unsigned char *)name);
	  if (cp != end)
	    hash->update(ctx, end - cp, cp);
	}
     
      hash->digest(ctx, hash->digest_size, digest);
      
      /* namebuff used for workspace above, restore to leave unchanged on exit */
      p = (unsigned char*)(rrset[0]);
      extract_name(header, plen, &p, name, 1, 0);

      if (key)
	{
	  if (algo_in == algo && keytag_in == key_tag &&
	      verify(key, keylen, sig, sig_len, digest, algo))
	    return STAT_SECURE;
	}
      else
	{
	  /* iterate through all possible keys 4035 5.3.1 */
	  for (; crecp; crecp = cache_find_by_name(crecp, keyname, now, F_DNSKEY))
	    if (crecp->addr.key.algo == algo && crecp->addr.key.keytag == key_tag &&
		verify(crecp->addr.key.keydata, crecp->uid, sig, sig_len, digest, algo))
	      return STAT_SECURE;
	}
    }

  return STAT_BOGUS;
}
 
/* The DNS packet is expected to contain the answer to a DNSKEY query.
   Leave name of query in name.
   Put all DNSKEYs in the answer which are valid into the cache.
   return codes:
         STAT_INSECURE bad packet, no DNSKEYs in reply.
	 STAT_SECURE   At least one valid DNSKEY found and in cache.
	 STAT_BOGUS    No DNSKEYs found, which  can be validated with DS,
	               or self-sign for DNSKEY RRset is not valid.
	 STAT_NEED_DS  DS records to validate a key not found, name in keyname 
*/
int dnssec_validate_by_ds(time_t now, struct dns_header *header, size_t plen, char *name, char *keyname, int class)
{
  unsigned char *psave, *p = (unsigned char *)(header+1);
  struct crec *crecp, *recp1;
  int rc, j, qtype, qclass, ttl, rdlen, flags, algo, valid, keytag;
  struct blockdata *key;

  if (ntohs(header->qdcount) != 1 ||
      !extract_name(header, plen, &p, name, 1, 4))
    {
      strcpy(name, "<none>");
      return STAT_INSECURE;
    }

  GETSHORT(qtype, p);
  GETSHORT(qclass, p);
  
  if (qtype != T_DNSKEY || qclass != class || ntohs(header->ancount) == 0)
    return STAT_INSECURE;

   /* See if we have cached a DS record which validates this key */
  if (!(crecp = cache_find_by_name(NULL, name, now, F_DS)))
    {
      strcpy(keyname, name);
      return STAT_NEED_DS;
    }

  cache_start_insert();

  /* NOTE, we need to find ONE DNSKEY which matches the DS */
  for (valid = 0, j = ntohs(header->ancount); j != 0; j--) 
    {
      /* Ensure we have type, class  TTL and length */
      if (!(rc = extract_name(header, plen, &p, name, 0, 10)))
	return STAT_INSECURE; /* bad packet */
  
      GETSHORT(qtype, p); 
      GETSHORT(qclass, p);
      GETLONG(ttl, p);
      GETSHORT(rdlen, p);

      if (qclass != class || qtype != T_DNSKEY || rc == 2)
	{
	  if (ADD_RDLEN(header, p, plen, rdlen))
	    continue;

	  return STAT_INSECURE; /* bad packet */
	}
      
      if (!CHECK_LEN(header, p, plen, rdlen) || rdlen < 4)
	return STAT_INSECURE; /* bad packet */
      
      psave = p;
      
      GETSHORT(flags, p);
      if (*p++ != 3)
	return STAT_INSECURE;
      algo = *p++;
      keytag = dnskey_keytag(algo, flags, p, rdlen - 4);
      
      /* Put the key into the cache. Note that if the validation fails, we won't
	 call cache_end_insert() and this will never be committed. */
      if ((key = blockdata_alloc((char*)p, rdlen - 4)) &&
	  (recp1 = cache_insert(name, NULL, now, ttl, F_FORWARD | F_DNSKEY)))
	{
	  recp1->uid = rdlen - 4;
	  recp1->addr.key.keydata = key;
	  recp1->addr.key.algo = algo;
	  recp1->addr.key.keytag = keytag;
	}
      
      p = psave;
      if (!ADD_RDLEN(header, p, plen, rdlen))
	return STAT_INSECURE; /* bad packet */
      
      /* Already determined that message is OK. Just loop stuffing cache */ 
      if (valid || !key)
	continue;
      
      for (recp1 = crecp; recp1; recp1 = cache_find_by_name(recp1, name, now, F_DS))
	{
	  void *ctx;
	  unsigned char *digest, *ds_digest;
	  const struct nettle_hash *hash;
	  
	  if (recp1->addr.key.algo == algo && 
	      recp1->addr.key.keytag == keytag &&
	      (flags & 0x100) && /* zone key flag */
	      (hash = hash_find(ds_digest_name(recp1->addr.key.digest))) &&
	      hash_init(hash, &ctx, &digest))
	    
	    {
	      int wire_len = to_wire(name);
	      
	      /* Note that digest may be different between DSs, so 
		 we can't move this outside the loop. */
	      hash->update(ctx, (unsigned int)wire_len, (unsigned char *)name);
	      hash->update(ctx, (unsigned int)rdlen, psave);
	      hash->digest(ctx, hash->digest_size, digest);
	      
	      from_wire(name);
	      
	      if (recp1->uid == (int)hash->digest_size &&
		  (ds_digest = blockdata_retrieve(recp1->addr.key.keydata, recp1->uid, NULL)) &&
		  memcmp(ds_digest, digest, recp1->uid) == 0 &&
		  validate_rrset(now, header, plen, class, T_DNSKEY, name, keyname, key, rdlen - 4, algo, keytag))
		{
		  struct all_addr a;
		  valid = 1;
		  a.addr.keytag = keytag;
		  log_query(F_KEYTAG | F_UPSTREAM, name, &a, "DNSKEY keytag %u");
		  break;
		}
	    }
	}
    }

  if (valid)
    {
      /* commit cache insert. */
      cache_end_insert();
      return STAT_SECURE;
    }

  log_query(F_UPSTREAM, name, NULL, "BOGUS DNSKEY");
  return STAT_BOGUS;
}

/* The DNS packet is expected to contain the answer to a DS query
   Leave name of DS query in name.
   Put all DSs in the answer which are valid into the cache.
   return codes:
   STAT_INSECURE    bad packet, no DS in reply.
   STAT_SECURE      At least one valid DS found and in cache.
   STAT_BOGUS       At least one DS found, which fails validation.
   STAT_NEED_DNSKEY DNSKEY records to validate a DS not found, name in keyname
*/

int dnssec_validate_ds(time_t now, struct dns_header *header, size_t plen, char *name, char *keyname, int class)
{
  unsigned char *psave, *p = (unsigned char *)(header+1);
  struct crec *crecp;
  int qtype, qclass, val, j;
  struct blockdata *key;

  if (ntohs(header->qdcount) != 1 ||
      !extract_name(header, plen, &p, name, 1, 4))
    {
      strcpy(name, "<none>");
      return STAT_INSECURE;
    }

  GETSHORT(qtype, p);
  GETSHORT(qclass, p);

  if (qtype != T_DS || qclass != class || ntohs(header->ancount) == 0)
    return STAT_INSECURE;
  
  val = validate_rrset(now, header, plen, class, T_DS, name, keyname, NULL, 0, 0, 0);
 
  if (val == STAT_BOGUS)
    log_query(F_UPSTREAM, name, NULL, "BOGUS DS");

  /* failed to validate or missing key. */
  if (val != STAT_SECURE)
    return val;
  
  cache_start_insert();

  for (j = ntohs(header->ancount); j != 0; j--) 
    {
      int ttl, rdlen, rc, algo, digest, keytag;
      
      /* Ensure we have type, class  TTL and length */
      if (!(rc = extract_name(header, plen, &p, name, 0, 10)))
	return STAT_INSECURE; /* bad packet */
      
      GETSHORT(qtype, p); 
      GETSHORT(qclass, p);
      GETLONG(ttl, p);
      GETSHORT(rdlen, p);
      
      /* check type, class and name, skip if not in DS rrset */
      if (qclass == class && qtype == T_DS && rc == 1)
	{
	  if (!CHECK_LEN(header, p, plen, rdlen) || rdlen < 4)
	    return STAT_INSECURE; /* bad packet */
	  
	  psave = p;
	  GETSHORT(keytag, p);
	  algo = *p++;
	  digest = *p++;
	  
	  /* We've proved that the DS is OK, store it in the cache */
	  if ((key = blockdata_alloc((char*)p, rdlen - 4)) &&
	      (crecp = cache_insert(name, NULL, now, ttl, F_FORWARD | F_DS)))
	    {
	      struct all_addr a;
	      a.addr.keytag = keytag;
	      log_query(F_KEYTAG | F_UPSTREAM, name, &a, "DS keytag %u");
	      crecp->addr.key.digest = digest;
	      crecp->addr.key.keydata = key;
	      crecp->addr.key.algo = algo;
	      crecp->addr.key.keytag = keytag;
	      crecp->uid = rdlen - 4; 
	    }
	  else
	    return STAT_INSECURE; /* cache problem */
	  
	  p = psave;
	}

      if (!ADD_RDLEN(header, p, plen, rdlen))
	return STAT_INSECURE; /* bad packet */
     
    }
  
  cache_end_insert();  
  
  return STAT_SECURE;
}

/* Validate all the RRsets in the answer and authority sections of the reply (4035:3.2.3) */
int dnssec_validate_reply(time_t now, struct dns_header *header, size_t plen, char *name, char *keyname, int *class)
{
  unsigned char *ans_start, *p1, *p2;
  int type1, class1, rdlen1, type2, class2, rdlen2;
  int i, j, rc;

  if (!(ans_start = skip_questions(header, plen)))
    return STAT_INSECURE;
   
  for (p1 = ans_start, i = 0; i < ntohs(header->ancount) + ntohs(header->nscount); i++)
    {
      if (!extract_name(header, plen, &p1, name, 1, 10))
	return STAT_INSECURE; /* bad packet */
      
      GETSHORT(type1, p1);
      GETSHORT(class1, p1);
      p1 += 4; /* TTL */
      GETSHORT(rdlen1, p1);
      
      /* Don't try and validate RRSIGs! */
      if (type1 != T_RRSIG)
	{
	  /* Check if we've done this RRset already */
	  for (p2 = ans_start, j = 0; j < i; j++)
	    {
	      if (!(rc = extract_name(header, plen, &p2, name, 0, 10)))
		return STAT_INSECURE; /* bad packet */
	      
	      GETSHORT(type2, p2);
	      GETSHORT(class2, p2);
	      p2 += 4; /* TTL */
	      GETSHORT(rdlen2, p2);
	      
	      if (type2 == type1 && class2 == class1 && rc == 1)
		break; /* Done it before: name, type, class all match. */
	      
	      if (!ADD_RDLEN(header, p2, plen, rdlen2))
		return STAT_INSECURE;
	    }
	  
	  /* Not done, validate now */
	  if (j == i && (rc = validate_rrset(now, header, plen, class1, type1, name, keyname, NULL, 0, 0, 0)) != STAT_SECURE)
	    {
	      *class = class1; /* Class for DS or DNSKEY */
	      return rc;
	    }
	}

      if (!ADD_RDLEN(header, p1, plen, rdlen1))
	return STAT_INSECURE;
    }

  return STAT_SECURE;
}


/* Compute keytag (checksum to quickly index a key). See RFC4034 */
int dnskey_keytag(int alg, int flags, unsigned char *key, int keylen)
{
  if (alg == 1)
    {
      /* Algorithm 1 (RSAMD5) has a different (older) keytag calculation algorithm.
         See RFC4034, Appendix B.1 */
      return key[keylen-4] * 256 + key[keylen-3];
    }
  else
    {
      unsigned long ac;
      int i;

      ac = ((htons(flags) >> 8) | ((htons(flags) << 8) & 0xff00)) + 0x300 + alg;
      for (i = 0; i < keylen; ++i)
        ac += (i & 1) ? key[i] : key[i] << 8;
      ac += (ac >> 16) & 0xffff;
      return ac & 0xffff;
    }
}

size_t dnssec_generate_query(struct dns_header *header, char *end, char *name, int class, int type, union mysockaddr *addr)
{
  unsigned char *p;
  char types[20];
  
  querystr("dnssec", types, type);

  if (addr->sa.sa_family == AF_INET) 
    log_query(F_DNSSEC | F_IPV4, name, (struct all_addr *)&addr->in.sin_addr, types);
#ifdef HAVE_IPV6
  else
    log_query(F_DNSSEC | F_IPV6, name, (struct all_addr *)&addr->in6.sin6_addr, types);
#endif
  
  header->qdcount = htons(1);
  header->ancount = htons(0);
  header->nscount = htons(0);
  header->arcount = htons(0);

  header->hb3 = HB3_RD; 
  SET_OPCODE(header, QUERY);
  header->hb4 = HB4_CD;

  /* ID filled in later */

  p = (unsigned char *)(header+1);
	
  p = do_rfc1035_name(p, name);
  *p++ = 0;
  PUTSHORT(type, p);
  PUTSHORT(class, p);

  return add_do_bit(header, p - (unsigned char *)header, end);
}
  
#endif /* HAVE_DNSSEC */
