/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * Nemo
 *
 * Copyright (C) 2024 Nemo Developers
 *
 * Nemo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Nemo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, MA 02110-1335, USA.
 */

#ifndef NEMO_COLUMN_VIEW_H
#define NEMO_COLUMN_VIEW_H

#include "nemo-view.h"

#define NEMO_COLUMN_VIEW_ID "OAFIID:Nemo_File_Manager_Column_View"

#define NEMO_TYPE_COLUMN_VIEW nemo_column_view_get_type()
#define NEMO_COLUMN_VIEW(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST ((obj), NEMO_TYPE_COLUMN_VIEW, NemoColumnView))
#define NEMO_COLUMN_VIEW_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST ((klass), NEMO_TYPE_COLUMN_VIEW, NemoColumnViewClass))
#define NEMO_IS_COLUMN_VIEW(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE ((obj), NEMO_TYPE_COLUMN_VIEW))
#define NEMO_IS_COLUMN_VIEW_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE ((klass), NEMO_TYPE_COLUMN_VIEW))
#define NEMO_COLUMN_VIEW_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS ((obj), NEMO_TYPE_COLUMN_VIEW, NemoColumnViewClass))

typedef struct _NemoColumnView NemoColumnView;
typedef struct _NemoColumnViewPriv NemoColumnViewPriv;
typedef struct _NemoColumnViewClass NemoColumnViewClass;

struct _NemoColumnView {
	NemoView parent;
	NemoColumnViewPriv *priv;
};

struct _NemoColumnViewClass {
	NemoViewClass parent_class;
};

GType nemo_column_view_get_type (void);
void nemo_column_view_register (void);

#endif /* NEMO_COLUMN_VIEW_H */
