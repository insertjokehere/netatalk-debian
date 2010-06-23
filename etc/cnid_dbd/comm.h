/*
 * $Id: comm.h,v 1.5 2009-10-19 08:09:07 didg Exp $
 *
 * Copyright (C) Joerg Lenneis 2003
 * All Rights Reserved.  See COPYING.
 */

#ifndef CNID_DBD_COMM_H
#define CNID_DBD_COMM_H 1


#include <atalk/cnid_dbd_private.h>


extern int      comm_init  (struct db_param *, int, int);
extern int      comm_rcv  (struct cnid_dbd_rqst *,  time_t, const sigset_t *, time_t *);
extern int      comm_snd  (struct cnid_dbd_rply *);
extern int      comm_nbe  (void);

#endif /* CNID_DBD_COMM_H */

