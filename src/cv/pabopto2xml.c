#ifndef lint
static const char RCSid[] = "$Id: pabopto2xml.c,v 2.1 2012/08/24 15:20:18 greg Exp $";
#endif
/*
 * Convert PAB-Opto measurements to XML format using tensor tree representation
 * Employs Bonneel et al. Earth Mover's Distance interpolant.
 *
 *	G.Ward
 */

#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "bsdf.h"

#ifndef GRIDRES
#define GRIDRES		200		/* max. grid resolution per side */
#endif

#define RSCA		1.9		/* radius scaling factor (empirical) */
#define MSCA		.12		/* magnitude scaling (empirical) */

typedef struct {
	float		vsum;		/* BSDF sum */
	unsigned short	nval;		/* number of values in sum */
	unsigned short	hrad2;		/* half radius squared */
} GRIDVAL;			/* grid value */

typedef struct {
	float		bsdf;		/* BSDF value at peak */
	unsigned short	rad;		/* radius */
	unsigned char	gx, gy;		/* grid position */
} RBFVAL;			/* radial basis function value */

typedef struct s_rbflist {
	struct s_rbflist	*next;		/* next in our RBF list */
	FVECT			invec;		/* incident vector direction */
	int			nrbf;		/* number of RBFs */
	RBFVAL			rbfa[1];	/* RBF array (extends struct) */
} RBFLIST;			/* RBF representation of BSDF @ 1 incidence */

				/* our loaded grid for this incident angle */
static double	theta_in_deg, phi_in_deg;
static GRIDVAL	bsdf_grid[GRIDRES][GRIDRES];

				/* processed incident BSDF measurements */
static RBFLIST	*bsdf_list = NULL;

/* Count up non-empty nodes and build RBF representation from current grid */ 
static RBFLIST *
make_rbfrep(void)
{
	int	nn = 0;
	RBFLIST	*newnode;
	int	i, j;
				/* count non-empty bins */
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++)
		nn += (bsdf_grid[i][j].nval > 0);
				/* allocate RBF array */
	newnode = (RBFLIST *)malloc(sizeof(RBFLIST) + sizeof(RBFVAL)*(nn-1));
	if (newnode == NULL) {
		fputs("Out of memory in make_rbfrep\n", stderr);
		exit(1);
	}
	newnode->invec[2] = sin(M_PI/180.*theta_in_deg);
	newnode->invec[0] = cos(M_PI/180.*phi_in_deg)*newnode->invec[2];
	newnode->invec[1] = sin(M_PI/180.*phi_in_deg)*newnode->invec[2];
	newnode->invec[2] = sqrt(1. - newnode->invec[2]*newnode->invec[2]);
	newnode->nrbf = nn;
	nn = 0;			/* fill RBF array */
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++)
		if (bsdf_grid[i][j].nval) {
			newnode->rbfa[nn].bsdf = MSCA*bsdf_grid[i][j].vsum /
						(double)bsdf_grid[i][j].nval;
			newnode->rbfa[nn].rad =
				(int)(2.*RSCA*sqrt((double)bsdf_grid[i][j].hrad2) + .5);
			newnode->rbfa[nn].gx = i;
			newnode->rbfa[nn].gy = j;
			++nn;
		}
	newnode->next = bsdf_list;
	return(bsdf_list = newnode);
}

/* Compute grid position from normalized outgoing vector */
static void
pos_from_vec(int pos[2], const FVECT vec)
{
	double	sq[2];		/* uniform hemispherical projection */
	double	norm = 1./sqrt(1. + vec[2]);

	SDdisk2square(sq, vec[0]*norm, vec[1]*norm);

	pos[0] = (int)(sq[0]*GRIDRES);
	pos[1] = (int)(sq[1]*GRIDRES);
}

/* Compute outgoing vector from grid position */
static void
vec_from_pos(FVECT vec, int xpos, int ypos)
{
	double	uv[2];
	double	r2;
	
	SDsquare2disk(uv, (1./GRIDRES)*(xpos+.5), (1./GRIDRES)*(ypos+.5));
				/* uniform hemispherical projection */
	r2 = uv[0]*uv[0] + uv[1]*uv[1];
	vec[0] = vec[1] = sqrt(2. - r2);
	vec[0] *= uv[0];
	vec[1] *= uv[1];
	vec[2] = 1. - r2;
}

/* Evaluate RBF at this grid position */
static double
eval_rbfrep2(const RBFLIST *rp, int xi, int yi)
{
	double		res = .0;
	const RBFVAL	*rbfp;
	double		sig2;
	int		x2, y2;
	int		n;

	rbfp = rp->rbfa;
	for (n = rp->nrbf; n--; rbfp++) {
		x2 = (signed)rbfp->gx - xi;
		x2 *= x2;
		y2 = (signed)rbfp->gy - yi;
		y2 *= y2;
		sig2 = -.5*(x2 + y2)/(double)(rbfp->rad*rbfp->rad);
		if (sig2 > -19.)
			res += rbfp->bsdf * exp(sig2);
	}
	return(res);
}

/* Evaluate RBF for BSDF at the given normalized outgoing direction */
static double
eval_rbfrep(const RBFLIST *rp, const FVECT outvec)
{
	double		res = .0;
	const RBFVAL	*rbfp;
	FVECT		odir;
	double		sig2;
	int		n;

	rbfp = rp->rbfa;
	for (n = rp->nrbf; n--; rbfp++) {
		vec_from_pos(odir, rbfp->gx, rbfp->gy);
		sig2 = (DOT(odir, outvec) - 1.) /
				((M_PI*M_PI/(double)(GRIDRES*GRIDRES)) *
						rbfp->rad*rbfp->rad);
		if (sig2 > -19.)
			res += rbfp->bsdf * exp(sig2);
	}
	return(res);
}

/* Load a set of measurements corresponding to a particular incident angle */
static int
load_bsdf_meas(const char *fname)
{
	FILE	*fp = fopen(fname, "r");
	int	inp_is_DSF = -1;
	double	theta_out, phi_out, val;
	char	buf[2048];
	int	n, c;
	
	if (fp == NULL) {
		fputs(fname, stderr);
		fputs(": cannot open\n", stderr);
		return(0);
	}
	memset(bsdf_grid, 0, sizeof(bsdf_grid));
				/* read header information */
	while ((c = getc(fp)) == '#' || c == EOF) {
		if (fgets(buf, sizeof(buf), fp) == NULL) {
			fputs(fname, stderr);
			fputs(": unexpected EOF\n", stderr);
			fclose(fp);
			return(0);
		}
		if (!strcmp(buf, "format: theta phi DSF\n")) {
			inp_is_DSF = 1;
			continue;
		}
		if (!strcmp(buf, "format: theta phi BSDF\n")) {
			inp_is_DSF = 0;
			continue;
		}
		if (sscanf(buf, "intheta %lf", &theta_in_deg) == 1)
			continue;
		if (sscanf(buf, "inphi %lf", &phi_in_deg) == 1)
			continue;
		if (sscanf(buf, "incident_angle %lf %lf",
				&theta_in_deg, &phi_in_deg) == 2)
			continue;
	}
	if (inp_is_DSF < 0) {
		fputs(fname, stderr);
		fputs(": unknown format\n", stderr);
		fclose(fp);
		return(0);
	}
	ungetc(c, fp);		/* read actual data */
	while (fscanf(fp, "%lf %lf %lf\n", &theta_out, &phi_out, &val) == 3) {
		FVECT	ovec;
		int	pos[2];

		ovec[2] = sin(M_PI/180.*theta_out);
		ovec[0] = cos(M_PI/180.*phi_out) * ovec[2];
		ovec[1] = sin(M_PI/180.*phi_out) * ovec[2];
		ovec[2] = sqrt(1. - ovec[2]*ovec[2]);

		if (inp_is_DSF)
			val /= ovec[2];	/* convert from DSF to BSDF */

		pos_from_vec(pos, ovec);

		bsdf_grid[pos[0]][pos[1]].vsum += val;
		bsdf_grid[pos[0]][pos[1]].nval++;
	}
	n = 0;
	while ((c = getc(fp)) != EOF)
		n += !isspace(c);
	if (n) 
		fprintf(stderr,
			"%s: warning: %d unexpected characters past EOD\n",
				fname, n);
	fclose(fp);
	return(1);
}

/* Compute radii for non-empty bins */
/* (distance to furthest empty bin for which non-empty bin is the closest) */
static void
compute_radii(void)
{
	unsigned char	fill_grid[GRIDRES][GRIDRES];
	int		r, r2, lastr2;
	int		i, j, jn, ii, jj, inear, jnear;
						/* proceed in zig-zag */
	lastr2 = GRIDRES*GRIDRES;
	for (i = 0; i < GRIDRES; i++)
	    for (jn = 0; jn < GRIDRES; jn++) {
		j = (i&1) ? jn : GRIDRES-1-jn;
		if (bsdf_grid[i][j].nval)	/* find empty grid pos. */
			continue;
		r = (int)sqrt((double)lastr2) + 2;
		inear = jnear = -1;		/* find nearest non-empty */
		lastr2 = 2*GRIDRES*GRIDRES;
		for (ii = i-r; ii <= i+r; ii++) {
		    if (ii < 0) continue;
		    if (ii >= GRIDRES) break;
		    for (jj = j-r; jj <= j+r; jj++) {
			if (jj < 0) continue;
			if (jj >= GRIDRES) break;
			if (!bsdf_grid[ii][jj].nval)
				continue;
			r2 = (ii-i)*(ii-i) + (jj-j)*(jj-j);
			if (r2 >= lastr2)
				continue;
			lastr2 = r2;
			inear = ii; jnear = jj;
		    }
		}
						/* record if > previous */
		if (bsdf_grid[inear][jnear].hrad2 < lastr2)
			bsdf_grid[inear][jnear].hrad2 = lastr2;
	    }
						/* fill in others */
	memset(fill_grid, 0, sizeof(fill_grid));
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++) {
		if (!bsdf_grid[i][j].nval)
			continue;
		if (bsdf_grid[i][j].hrad2)
			continue;
		r = GRIDRES/20;
		lastr2 = 2*r*r;
		for (ii = i-r; ii <= i+r; ii++) {
		    if (ii < 0) continue;
		    if (ii >= GRIDRES) break;
		    for (jj = j-r; jj <= j+r; jj++) {
			if (jj < 0) continue;
			if (jj >= GRIDRES) break;
			if (!bsdf_grid[ii][jj].hrad2)
				continue;
			r2 = (ii-i)*(ii-i) + (jj-j)*(jj-j);
			if (r2 >= lastr2)
				continue;
			fill_grid[i][j] = bsdf_grid[ii][jj].hrad2;
			lastr2 = r2;
		    }
		}
	    }
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++)
		if (fill_grid[i][j])
			bsdf_grid[i][j].hrad2 = fill_grid[i][j];
}

/* Cull points for more uniform distribution */
static void
cull_values(void)
{
	int	i, j, ii, jj, r, r2;
						/* simple greedy algorithm */
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++) {
		if (!bsdf_grid[i][j].nval)
			continue;
		if (!bsdf_grid[i][j].hrad2)
			continue;
		r = (int)(2.*sqrt((double)bsdf_grid[i][j].hrad2) + .9999);
		for (ii = i-r; ii <= i+r; ii++) {
		    if (ii < 0) continue;
		    if (ii >= GRIDRES) break;
		    for (jj = j-r; jj <= j+r; jj++) {
			if (jj < 0) continue;
			if (jj >= GRIDRES) break;
			if (!bsdf_grid[ii][jj].nval)
				continue;
			r2 = (ii-i)*(ii-i) + (jj-j)*(jj-j);
			if (!r2 | (r2 > r*r))
				continue;
						/* absorb victim's value */
			bsdf_grid[i][j].vsum += bsdf_grid[ii][jj].vsum;
			bsdf_grid[i][j].nval += bsdf_grid[ii][jj].nval;
			memset(&bsdf_grid[ii][jj], 0, sizeof(GRIDVAL));
		    }
		}
	    }
}


#if 1
/* Test main produces a Radiance model from the given input file */
int
main(int argc, char *argv[])
{
	char	buf[128];
	FILE	*pfp;
	double	bsdf;
	FVECT	dir;
	int	i, j, n;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s input.dat > output.rad\n", argv[0]);
		return(1);
	}
	if (!load_bsdf_meas(argv[1]))
		return(1);
						/* produce spheres at meas. */
	puts("void plastic orange\n0\n0\n5 .6 .4 .01 .04 .08\n");
	n = 0;
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++)
		if (bsdf_grid[i][j].nval) {
			double	bsdf = bsdf_grid[i][j].vsum /
					(double)bsdf_grid[i][j].nval;
			FVECT	dir;

			vec_from_pos(dir, i, j);
			printf("orange sphere s%04d\n0\n0\n", ++n);
			printf("4 %.6g %.6g %.6g .0015\n\n",
					dir[0]*bsdf, dir[1]*bsdf, dir[2]*bsdf);
		}
	compute_radii();
	cull_values();
						/* highlight chosen values */
	puts("void plastic pink\n0\n0\n5 .5 .05 .9 .04 .08\n");
	n = 0;
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++)
		if (bsdf_grid[i][j].nval) {
			bsdf = bsdf_grid[i][j].vsum /
					(double)bsdf_grid[i][j].nval;
			vec_from_pos(dir, i, j);
			printf("pink cone c%04d\n0\n0\n8\n", ++n);
			printf("\t%.6g %.6g %.6g\n",
					dir[0]*bsdf, dir[1]*bsdf, dir[2]*bsdf);
			printf("\t%.6g %.6g %.6g\n",
					dir[0]*(bsdf+.005), dir[1]*(bsdf+.005),
					dir[2]*(bsdf+.005));
			puts("\t.003\t0\n");
		}
						/* output continuous surface */
	make_rbfrep();
	puts("void trans tgreen\n0\n0\n7 .7 1 .7 .04 .04 .9 .9\n");
	fflush(stdout);
	sprintf(buf, "gensurf tgreen bsdf - - - %d %d", GRIDRES, GRIDRES);
	pfp = popen(buf, "w");
	if (pfp == NULL) {
		fputs(buf, stderr);
		fputs(": cannot start command\n", stderr);
		return(1);
	}
	for (i = 0; i < GRIDRES; i++)
	    for (j = 0; j < GRIDRES; j++) {
		vec_from_pos(dir, i, j);
		bsdf = eval_rbfrep(bsdf_list, dir);
		fprintf(pfp, "%.8e %.8e %.8e\n",
				dir[0]*bsdf, dir[1]*bsdf, dir[2]*bsdf);
	    }
	return(pclose(pfp)==0 ? 0 : 1);
}
#endif