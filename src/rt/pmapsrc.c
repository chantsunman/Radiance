#ifndef lint
static const char RCSid[] = "$Id: pmapsrc.c,v 2.17 2018/11/08 00:54:07 greg Exp $";
#endif
/* 
   ==================================================================
   Photon map support routines for emission from light sources

   Roland Schregle (roland.schregle@{hslu.ch, gmail.com})
   (c) Fraunhofer Institute for Solar Energy Systems,
   (c) Lucerne University of Applied Sciences and Arts,
   supported by the Swiss National Science Foundation (SNSF, #147053)
   ==================================================================
 
*/



#include "pmapsrc.h"
#include "pmap.h"
#include "pmaprand.h"
#include "otypes.h"
#include "otspecial.h"

/* List of photon port modifier names */
char *photonPortList [MAXSET + 1] = {NULL};
/* Photon port objects (with modifiers in photonPortMods) */
SRCREC *photonPorts = NULL;
unsigned numPhotonPorts = 0;

void (*photonPartition [NUMOTYPE]) (EmissionMap*);
void (*photonOrigin [NUMOTYPE]) (EmissionMap*);

   

static int flatPhotonPartition2 (EmissionMap* emap, unsigned long mp, 
                                 FVECT cent, FVECT u, FVECT v, 
                                 double du2, double dv2)
/* Recursive part of flatPhotonPartition(..) */
{
   FVECT newct, newax;
   unsigned long npl, npu;

   if (mp > emap -> maxPartitions) {
      /* Enlarge partition array */
      emap -> maxPartitions <<= 1;
      emap -> partitions = (unsigned char*)realloc(emap -> partitions, 
                                           emap -> maxPartitions >> 1);

      if (!emap -> partitions) 
         error(USER, "can't allocate source partitions");

      memset(emap -> partitions + (emap -> maxPartitions >> 2), 0,
             emap -> maxPartitions >> 2);
   }
   
   if (du2 * dv2 <= 1) {                /* hit limit? */
      setpart(emap -> partitions, emap -> partitionCnt, S0);
      emap -> partitionCnt++;
      return 1;      
   }
   
   if (du2 > dv2) {                     /* subdivide in U */
      setpart(emap -> partitions, emap -> partitionCnt, SU);
      emap -> partitionCnt++;
      newax [0] = 0.5 * u [0];
      newax [1] = 0.5 * u [1];
      newax [2] = 0.5 * u [2];
      u = newax;
      du2 *= 0.25;
   } 
   
   else {                               /* subdivide in V */
      setpart(emap -> partitions, emap -> partitionCnt, SV);
      emap -> partitionCnt++;
      newax [0] = 0.5 * v [0];
      newax [1] = 0.5 * v [1];
      newax [2] = 0.5 * v [2];
      v = newax;
      dv2 *= 0.25;
   }
   
   /* lower half */
   newct [0] = cent [0] - newax [0];
   newct [1] = cent [1] - newax [1];
   newct [2] = cent [2] - newax [2];
   npl = flatPhotonPartition2(emap, mp << 1, newct, u, v, du2, dv2);
   
   /* upper half */
   newct [0] = cent [0] + newax [0];
   newct [1] = cent [1] + newax [1];
   newct [2] = cent [2] + newax [2];
   npu = flatPhotonPartition2(emap, mp << 1, newct, u, v, du2, dv2);
   
   /* return total */
   return npl + npu;
}



static void flatPhotonPartition (EmissionMap* emap)
/* Partition flat source for photon emission */
{
   RREAL    *vp;
   double   du2, dv2;

   memset(emap -> partitions, 0, emap -> maxPartitions >> 1);
   emap -> partArea = srcsizerat * thescene.cusize;
   emap -> partArea *= emap -> partArea;
   vp = emap -> src -> ss [SU];
   du2 = DOT(vp, vp) / emap -> partArea;
   vp = emap -> src -> ss [SV];
   dv2 = DOT(vp, vp) / emap -> partArea;
   emap -> partitionCnt = 0;
   emap -> numPartitions = flatPhotonPartition2(emap, 1, emap -> src -> sloc, 
                                                emap -> src -> ss [SU], 
                                                emap -> src -> ss [SV], 
                                                du2, dv2);
   emap -> partitionCnt = 0;
   emap -> partArea = emap -> src -> ss2 / emap -> numPartitions;
}



static void sourcePhotonPartition (EmissionMap* emap)
/* Partition scene cube faces or photon port for photon emission from 
   distant source */
{   
   if (emap -> port) {
      /* Partition photon port */
      SRCREC *src = emap -> src;
      emap -> src = emap -> port;
      photonPartition [emap -> src -> so -> otype] (emap);
      emap -> src = src;
   }
   
   else {
      /* No photon ports defined, so partition scene cube faces */
      memset(emap -> partitions, 0, emap -> maxPartitions >> 1);
      setpart(emap -> partitions, 0, S0);
      emap -> partitionCnt = 0;
      emap -> numPartitions = 1 / srcsizerat;
      emap -> numPartitions *= emap -> numPartitions;
      emap -> partArea = sqr(thescene.cusize) / emap -> numPartitions;
      emap -> numPartitions *= 6;  
   }
}



static void spherePhotonPartition (EmissionMap* emap)
/* Partition spherical source into equal solid angles using uniform
   mapping for photon emission */
{
   unsigned numTheta, numPhi;
   
   memset(emap -> partitions, 0, emap -> maxPartitions >> 1);
   setpart(emap -> partitions, 0, S0);
   emap -> partArea = 4 * PI * sqr(emap -> src -> srad);
   emap -> numPartitions = emap -> partArea / 
                           sqr(srcsizerat * thescene.cusize);

   numTheta = max(sqrt(2 * emap -> numPartitions / PI) + 0.5, 1);
   numPhi = 0.5 * PI * numTheta + 0.5;
   
   emap -> numPartitions = (unsigned long)numTheta * numPhi;
   emap -> partitionCnt = 0;
   emap -> partArea /= emap -> numPartitions;
}



static int cylPhotonPartition2 (EmissionMap* emap, unsigned long mp, 
                                FVECT cent, FVECT axis, double d2)
/* Recursive part of cyPhotonPartition(..) */
{
   FVECT newct, newax;
   unsigned long npl, npu;

   if (mp > emap -> maxPartitions) {
      /* Enlarge partition array */
      emap -> maxPartitions <<= 1;
      emap -> partitions = (unsigned char*)realloc(emap -> partitions,
                                                   emap -> maxPartitions >> 1);
      if (!emap -> partitions) 
         error(USER, "can't allocate source partitions");
         
      memset(emap -> partitions + (emap -> maxPartitions >> 2), 0,
            emap -> maxPartitions >> 2);
   }
   
   if (d2 <= 1) {
      /* hit limit? */
      setpart(emap -> partitions, emap -> partitionCnt, S0);
      emap -> partitionCnt++;
      return 1;
   }
   
   /* subdivide */
   setpart(emap -> partitions, emap -> partitionCnt, SU);
   emap -> partitionCnt++;
   newax [0] = 0.5 * axis [0];
   newax [1] = 0.5 * axis [1];
   newax [2] = 0.5 * axis [2];
   d2 *= 0.25;
   
   /* lower half */
   newct [0] = cent [0] - newax [0];
   newct [1] = cent [1] - newax [1];
   newct [2] = cent [2] - newax [2];
   npl = cylPhotonPartition2(emap, mp << 1, newct, newax, d2);
   
   /* upper half */
   newct [0] = cent [0] + newax [0];
   newct [1] = cent [1] + newax [1];
   newct [2] = cent [2] + newax [2];
   npu = cylPhotonPartition2(emap, mp << 1, newct, newax, d2);
   
   /* return total */
   return npl + npu;
}



static void cylPhotonPartition (EmissionMap* emap)
/* Partition cylindrical source for photon emission */
{
   double d2;

   memset(emap -> partitions, 0, emap -> maxPartitions >> 1);
   d2 = srcsizerat * thescene.cusize;
   d2 = PI * emap -> src -> ss2 / (2 * emap -> src -> srad * sqr(d2));
   d2 *= d2 * DOT(emap -> src -> ss [SU], emap -> src -> ss [SU]);

   emap -> partitionCnt = 0;
   emap -> numPartitions = cylPhotonPartition2(emap, 1, emap -> src -> sloc, 
                                               emap -> src -> ss [SU], d2);
   emap -> partitionCnt = 0;
   emap -> partArea = PI * emap -> src -> ss2 / emap -> numPartitions;
}



static void flatPhotonOrigin (EmissionMap* emap)
/* Init emission map with photon origin and associated surface axes on 
   flat light source surface. Also sets source aperture and sampling
   hemisphere axes at origin */
{
   int i, cent[3], size[3], parr[2];
   FVECT vpos;

   cent [0] = cent [1] = cent [2] = 0;
   size [0] = size [1] = size [2] = emap -> maxPartitions;
   parr [0] = 0; 
   parr [1] = emap -> partitionCnt;
   
   if (!skipparts(cent, size, parr, emap -> partitions))
      error(CONSISTENCY, "bad source partition in flatPhotonOrigin");
      
   vpos [0] = (1 - 2 * pmapRandom(partState)) * size [0] / 
              emap -> maxPartitions;
   vpos [1] = (1 - 2 * pmapRandom(partState)) * size [1] / 
              emap -> maxPartitions;
   vpos [2] = 0;
   
   for (i = 0; i < 3; i++) 
      vpos [i] += (double)cent [i] / emap -> maxPartitions;
      
   /* Get origin */
   for (i = 0; i < 3; i++) 
      emap -> photonOrg [i] = emap -> src -> sloc [i] + 
                              vpos [SU] * emap -> src -> ss [SU][i] + 
                              vpos [SV] * emap -> src -> ss [SV][i] + 
                              vpos [SW] * emap -> src -> ss [SW][i];
                              
   /* Get surface axes */
   VCOPY(emap -> us, emap -> src -> ss [SU]);
   normalize(emap -> us);
   VCOPY(emap -> ws, emap -> src -> ss [SW]);

   if (emap -> port) 
      /* Acts as a photon port; reverse normal as it points INSIDE per
       * mkillum convention */
      for (i = 0; i < 3; i++)
         emap -> ws [i] = -emap -> ws [i];
         
   fcross(emap -> vs, emap -> ws, emap -> us);
   
   /* Get hemisphere axes & aperture */
   if (emap -> src -> sflags & SSPOT) {
      VCOPY(emap -> wh, emap -> src -> sl.s -> aim);
      i = 0;
      
      do {
         emap -> vh [0] = emap -> vh [1] = emap -> vh [2] = 0;
         emap -> vh [i++] = 1;
         fcross(emap -> uh, emap -> vh, emap -> wh);
      } while (normalize(emap -> uh) < FTINY);
      
      fcross(emap -> vh, emap -> wh, emap -> uh);
      emap -> cosThetaMax = 1 - emap -> src -> sl.s -> siz / (2 * PI);
   }
   
   else {
      VCOPY(emap -> uh, emap -> us);
      VCOPY(emap -> vh, emap -> vs);
      VCOPY(emap -> wh, emap -> ws);
      emap -> cosThetaMax = 0;
   }
}



static void spherePhotonOrigin (EmissionMap* emap)
/* Init emission map with photon origin and associated surface axes on 
   spherical light source. Also sets source aperture and sampling
   hemisphere axes at origin */
{
   int i = 0;
   unsigned numTheta, numPhi, t, p;
   RREAL cosTheta, sinTheta, phi;

   /* Get current partition */
   numTheta = max(sqrt(2 * emap -> numPartitions / PI) + 0.5, 1);
   numPhi = 0.5 * PI * numTheta + 0.5;

   t = emap -> partitionCnt / numPhi;
   p = emap -> partitionCnt - t * numPhi;

   emap -> ws [2] = cosTheta = 1 - 2 * (t + pmapRandom(partState)) / numTheta;
   sinTheta = sqrt(1 - sqr(cosTheta));
   phi = 2 * PI * (p + pmapRandom(partState)) / numPhi;
   emap -> ws [0] = cos(phi) * sinTheta;
   emap -> ws [1] = sin(phi) * sinTheta;

   if (emap -> port) 
      /* Acts as a photon port; reverse normal as it points INSIDE per
       * mkillum convention */
      for (i = 0; i < 3; i++)
         emap -> ws [i] = -emap -> ws [i];   

   /* Get surface axes us & vs perpendicular to ws */
   do {
      emap -> vs [0] = emap -> vs [1] = emap -> vs [2] = 0;
      emap -> vs [i++] = 1;
      fcross(emap -> us, emap -> vs, emap -> ws);
   } while (normalize(emap -> us) < FTINY);

   fcross(emap -> vs, emap -> ws, emap -> us);
   
   /* Get origin */
   for (i = 0; i < 3; i++)
      emap -> photonOrg [i] = emap -> src -> sloc [i] + 
                              emap -> src -> srad * emap -> ws [i];
                              
   /* Get hemisphere axes & aperture */
   if (emap -> src -> sflags & SSPOT) {
      VCOPY(emap -> wh, emap -> src -> sl.s -> aim);
      i = 0;

      do {
         emap -> vh [0] = emap -> vh [1] = emap -> vh [2] = 0;
         emap -> vh [i++] = 1;
         fcross(emap -> uh, emap -> vh, emap -> wh);
      } while (normalize(emap -> uh) < FTINY);

      fcross(emap -> vh, emap -> wh, emap -> uh);
      emap -> cosThetaMax = 1 - emap -> src -> sl.s -> siz / (2 * PI);
   }

   else {
      VCOPY(emap -> uh, emap -> us);
      VCOPY(emap -> vh, emap -> vs);
      VCOPY(emap -> wh, emap -> ws);
      emap -> cosThetaMax = 0;
   }
}



static void sourcePhotonOrigin (EmissionMap* emap)
/* Init emission map with photon origin and associated surface axes 
   on scene cube face for distant light source. Also sets source 
   aperture (solid angle) and sampling hemisphere axes at origin */
{  
   unsigned long i, partsPerDim, partsPerFace;
   unsigned face;
   RREAL du, dv;                            
      
   if (emap -> port) {
      /* Get origin on photon port */
      SRCREC *src = emap -> src;
      emap -> src = emap -> port;
      photonOrigin [emap -> src -> so -> otype] (emap);
      emap -> src = src;
   }
   
   else {
      /* No ports defined, so get origin on scene cube face and SUFFA! */
      /* Get current face from partition number */
      partsPerDim = 1 / srcsizerat;
      partsPerFace = sqr(partsPerDim);
      face = emap -> partitionCnt / partsPerFace;

      if (!(emap -> partitionCnt % partsPerFace)) {
         /* Skipped to a new face; get its normal */
         emap -> ws [0] = emap -> ws [1] = emap -> ws [2] = 0;
         emap -> ws [face >> 1] = face & 1 ? 1 : -1;
         
         /* Get surface axes us & vs perpendicular to ws */
         face >>= 1;
         emap -> vs [0] = emap -> vs [1] = emap -> vs [2] = 0;
         emap -> vs [(face + (emap -> ws [face] > 0 ? 2 : 1)) % 3] = 1;
         fcross(emap -> us, emap -> vs, emap -> ws);
      }
      
      /* Get jittered offsets within face from partition number 
         (in range [-0.5, 0.5]) */
      i = emap -> partitionCnt % partsPerFace;
      du = (i / partsPerDim + pmapRandom(partState)) / partsPerDim - 0.5;
      dv = (i % partsPerDim + pmapRandom(partState)) / partsPerDim - 0.5;

      /* Jittered destination point within partition */
      for (i = 0; i < 3; i++)
         emap -> photonOrg [i] = thescene.cuorg [i] + 
                                 thescene.cusize * (0.5 + du * emap -> us [i] +
                                                    dv * emap -> vs [i] +
                                                    0.5 * emap -> ws [i]);
   }
   
   /* Get hemisphere axes & aperture */
   VCOPY(emap -> wh, emap -> src -> sloc);
   i = 0;
   
   do {
      emap -> vh [0] = emap -> vh [1] = emap -> vh [2] = 0;
      emap -> vh [i++] = 1;
      fcross(emap -> uh, emap -> vh, emap -> wh);
   } while (normalize(emap -> uh) < FTINY);
   
   fcross(emap -> vh, emap -> wh, emap -> uh);
   
   /* Get aperture */
   emap -> cosThetaMax = 1 - emap -> src -> ss2 / (2 * PI);
   emap -> cosThetaMax = min(1, max(-1, emap -> cosThetaMax));
}



static void cylPhotonOrigin (EmissionMap* emap)
/* Init emission map with photon origin and associated surface axes 
   on cylindrical light source surface. Also sets source aperture 
   and sampling hemisphere axes at origin */
{
   int i, cent[3], size[3], parr[2];
   FVECT v;

   cent [0] = cent [1] = cent [2] = 0;
   size [0] = size [1] = size [2] = emap -> maxPartitions;
   parr [0] = 0; 
   parr [1] = emap -> partitionCnt;

   if (!skipparts(cent, size, parr, emap -> partitions))
      error(CONSISTENCY, "bad source partition in cylPhotonOrigin");

   v [SU] = 0;
   v [SV] = (1 - 2 * pmapRandom(partState)) * (double)size [1] / 
            emap -> maxPartitions;
   v [SW] = (1 - 2 * pmapRandom(partState)) * (double)size [2] / 
            emap -> maxPartitions;
   normalize(v);
   v [SU] = (1 - 2 * pmapRandom(partState)) * (double)size [1] / 
            emap -> maxPartitions; 

   for (i = 0; i < 3; i++) 
      v [i] += (double)cent [i] / emap -> maxPartitions;

   /* Get surface axes */
   for (i = 0; i < 3; i++) 
      emap -> photonOrg [i] = emap -> ws [i] = 
                              (v [SV] * emap -> src -> ss [SV][i] + 
                               v [SW] * emap -> src -> ss [SW][i]) / 0.8559;

   if (emap -> port) 
      /* Acts as a photon port; reverse normal as it points INSIDE per
       * mkillum convention */
      for (i = 0; i < 3; i++)
         emap -> ws [i] = -emap -> ws [i];

   normalize(emap -> ws);
   VCOPY(emap -> us, emap -> src -> ss [SU]);
   normalize(emap -> us);
   fcross(emap -> vs, emap -> ws, emap -> us);

   /* Get origin */
   for (i = 0; i < 3; i++) 
      emap -> photonOrg [i] += v [SU] * emap -> src -> ss [SU][i] + 
                               emap -> src -> sloc [i];

   /* Get hemisphere axes & aperture */
   if (emap -> src -> sflags & SSPOT) {
      VCOPY(emap -> wh, emap -> src -> sl.s -> aim);
      i = 0;

      do {
         emap -> vh [0] = emap -> vh [1] = emap -> vh [2] = 0;
         emap -> vh [i++] = 1;
         fcross(emap -> uh, emap -> vh, emap -> wh);
      } while (normalize(emap -> uh) < FTINY);

      fcross(emap -> vh, emap -> wh, emap -> uh);
      emap -> cosThetaMax = 1 - emap -> src -> sl.s -> siz / (2 * PI);
   }

   else {
      VCOPY(emap -> uh, emap -> us);
      VCOPY(emap -> vh, emap -> vs);
      VCOPY(emap -> wh, emap -> ws);
      emap -> cosThetaMax = 0;
   }
}



void getPhotonPorts (char **portList)
/* Find geometry declared as photon ports from modifiers in portList */
{
   OBJECT i;
   OBJREC *obj, *mat;
   char **lp;   
   
   /* Init photon port objects */
   photonPorts = NULL;
   
   if (!portList [0])
      return;
   
   for (i = 0; i < nobjects; i++) {
      obj = objptr(i);
      mat = findmaterial(obj);
      
      /* Check if object is a surface and NOT a light source (duh) and
       * resolve its material via any aliases, then check for inclusion in
       * modifier list */
      if (issurface(obj -> otype) && mat && !islight(mat -> otype)) {
         for (lp = portList; *lp && strcmp(mat -> oname, *lp); lp++);
         
         if (*lp) {
            /* Add photon port */
            photonPorts = (SRCREC*)realloc(photonPorts, 
                                           (numPhotonPorts + 1) * 
                                           sizeof(SRCREC));
            if (!photonPorts) 
               error(USER, "can't allocate photon ports");
            
            photonPorts [numPhotonPorts].so = obj;
            photonPorts [numPhotonPorts].sflags = 0;
         
            if (!sfun [obj -> otype].of || !sfun[obj -> otype].of -> setsrc)
               objerror(obj, USER, "illegal photon port");
         
            setsource(photonPorts + numPhotonPorts, obj);
            numPhotonPorts++;
         }
      }
   }
   
   if (!numPhotonPorts)
      error(USER, "no valid photon ports found");
}



static void defaultEmissionFunc (EmissionMap* emap)
/* Default behaviour when no emission funcs defined for this source type */
{
   objerror(emap -> src -> so, INTERNAL, 
            "undefined photon emission function");
}



void initPhotonEmissionFuncs ()
/* Init photonPartition[] and photonOrigin[] dispatch tables */
{
   int i;
   
   for (i = 0; i < NUMOTYPE; i++) 
      photonPartition [i] = photonOrigin [i] = defaultEmissionFunc;
   
   photonPartition [OBJ_FACE] = photonPartition [OBJ_RING] = flatPhotonPartition;
   photonPartition [OBJ_SOURCE] = sourcePhotonPartition;
   photonPartition [OBJ_SPHERE] = spherePhotonPartition;
   photonPartition [OBJ_CYLINDER] = cylPhotonPartition;
   photonOrigin [OBJ_FACE] = photonOrigin [OBJ_RING] = flatPhotonOrigin;
   photonOrigin [OBJ_SOURCE] = sourcePhotonOrigin;
   photonOrigin [OBJ_SPHERE] = spherePhotonOrigin;
   photonOrigin [OBJ_CYLINDER] = cylPhotonOrigin;
}      



void initPhotonEmission (EmissionMap *emap, float numPdfSamples)
/* Initialize photon emission from partitioned light source emap -> src;
 * this involves integrating the flux emitted from the current photon
 * origin emap -> photonOrg and setting up a PDF to sample the emission
 * distribution with numPdfSamples samples */
{
   unsigned i, t, p;
   double phi, cosTheta, sinTheta, du, dv, dOmega, thetaScale;
   EmissionSample* sample;
   const OBJREC* mod =  findmaterial(emap -> src -> so);
   static RAY r;
#if 0   
   static double lastCosNorm = FHUGE;
   static SRCREC *lastSrc = NULL, *lastPort = NULL;
#endif   

   setcolor(emap -> partFlux, 0, 0, 0);

   photonOrigin [emap -> src -> so -> otype] (emap);
   cosTheta = DOT(emap -> ws, emap -> wh);

#if 0   
   if (emap -> src == lastSrc && emap -> port == lastPort &&        
       (emap -> src -> sflags & SDISTANT || mod -> omod == OVOID) &&
       cosTheta == lastCosNorm)
      /* Same source, port, and aperture-normal angle, and source is 
         either distant (and thus translationally invariant) or has
         no modifier --> flux unchanged */
      /* BUG: this optimisation ignores partial occlusion of ports and
         can lead to erroneous "zero emission" bailouts.
         It can also lead to bias with modifiers exhibiting high variance!
         Disabled for now -- RS 12/13 */         
      return;
      
   lastSrc = emap -> src;
   lastPort = emap -> port;
   lastCosNorm = cosTheta;      
#endif
         
   /* Need to recompute flux & PDF */
   emap -> cdf = 0;
   emap -> numSamples = 0;
   
   if (cosTheta <= 0 && 
       sqrt(1 - sqr(cosTheta)) <= emap -> cosThetaMax + FTINY)
      /* Aperture below surface; no emission from current origin */
      return;
   
   if (mod -> omod == OVOID && !emap -> port &&
       (cosTheta >= 1 - FTINY || (emap -> src -> sflags & SDISTANT && 
        acos(cosTheta) + acos(emap -> cosThetaMax) <= 0.5 * PI))) {
      /* Source is unmodified and has no port (which requires testing for 
         occlusion), and is either local with normal aligned aperture or 
         distant with aperture above surface; analytical flux & PDF */
      setcolor(emap -> partFlux, mod -> oargs.farg [0], 
               mod -> oargs.farg [1], mod -> oargs.farg [2]);

      /* Multiply radiance by Omega * dA to get flux */
      scalecolor(emap -> partFlux, 
                 PI * cosTheta * (1 - sqr(max(emap -> cosThetaMax, 0))) *
                 emap -> partArea);
   }
   
   else {
      /* Source is either modified, has a port, is local with off-normal 
         aperture, or distant with aperture partly below surface; get flux
         via numerical integration */
      thetaScale = (1 - emap -> cosThetaMax);
      
      /* Figga out numba of aperture samples for integration;
         numTheta / numPhi ratio is 1 / PI */
      du = sqrt(pdfSamples * 2 * thetaScale);
      emap -> numTheta = max(du + 0.5, 1);
      emap -> numPhi = max(PI * du + 0.5, 1);

      dOmega = 2 * PI * thetaScale / (emap -> numTheta * emap -> numPhi);
      thetaScale /= emap -> numTheta;
      
      /* Allocate PDF, baby */
      sample = emap -> samples = (EmissionSample*)
                                 realloc(emap -> samples, 
                                         sizeof(EmissionSample) * 
                                         emap -> numTheta * emap -> numPhi);
      if (!emap -> samples) 
         error(USER, "can't allocate emission PDF");     
         
      VCOPY(r.rorg, emap -> photonOrg);
      VCOPY(r.rop, emap -> photonOrg);
      r.rmax = 0;
      
      for (t = 0; t < emap -> numTheta; t++) {
         for (p = 0; p < emap -> numPhi; p++) {
            /* This uniform mapping handles 0 <= thetaMax <= PI */
            cosTheta = 1 - (t + pmapRandom(emitState)) * thetaScale;
            sinTheta = sqrt(1 - sqr(cosTheta));
            phi = 2 * PI * (p + pmapRandom(emitState)) / emap -> numPhi;
            du = cos(phi) * sinTheta;
            dv = sin(phi) * sinTheta;
            rayorigin(&r, PRIMARY, NULL, NULL);
            
            for (i = 0; i < 3; i++)
               r.rdir [i] = du * emap -> uh [i] + dv * emap -> vh [i] +
                            cosTheta * emap -> wh [i];            
                            
            /* Sample behind surface? */
            VCOPY(r.ron, emap -> ws);
            if ((r.rod = DOT(r.rdir, r.ron)) <= 0) 
               continue;
               
            /* Get radiance emitted in this direction; to get flux we 
               multiply by cos(theta_surface), dOmega, and dA. Ray 
               is directed towards light source for raytexture(). */
            if (!(emap -> src -> sflags & SDISTANT)) 
               for (i = 0; i < 3; i++) 
                  r.rdir [i] = -r.rdir [i];
                  
            /* Port occluded in this direction? */
            if (emap -> port && localhit(&r, &thescene)) 
               continue; 
               
            raytexture(&r, mod -> omod);
            setcolor(r.rcol, mod -> oargs.farg [0], mod -> oargs.farg [1], 
                     mod -> oargs.farg [2]);
            multcolor(r.rcol, r.pcol);
            
            /* Multiply by cos(theta_surface) */
            scalecolor(r.rcol, r.rod);
            
            /* Add PDF sample if nonzero; importance info for photon emission
             * could go here... */
            if (colorAvg(r.rcol)) {
               copycolor(sample -> pdf, r.rcol);
               sample -> cdf = emap -> cdf += colorAvg(sample -> pdf);
               sample -> theta = t;
               sample++ -> phi = p;
               emap -> numSamples++;
               addcolor(emap -> partFlux, r.rcol);
            }
         }
      }
      
      /* Multiply by dOmega * dA */
      scalecolor(emap -> partFlux, dOmega * emap -> partArea);
   }
}



#define vomitPhoton     emitPhoton
#define bluarrrghPhoton vomitPhoton

void emitPhoton (const EmissionMap* emap, RAY* ray)
/* Emit photon from current partition emap -> partitionCnt based on
   emission distribution. Returns new photon ray. */
{
   unsigned long i, lo, hi;
   const EmissionSample* sample = emap -> samples;
   RREAL du, dv, cosTheta, cosThetaSqr, sinTheta, phi;  
   const OBJREC* mod = findmaterial(emap -> src -> so);
   
   /* Choose a new origin within current partition for every 
      emitted photon to break up clustering artifacts */
   photonOrigin [emap -> src -> so -> otype] ((EmissionMap*)emap);
   /* If we have a local glow source with a maximum radius, then
      restrict our photon to the specified distance, otherwise we set
      the limit imposed by photonMaxDist (or no limit if 0) */
   if (mod -> otype == MAT_GLOW && !(emap -> src -> sflags & SDISTANT)
		&& mod -> oargs.farg[3] > FTINY)
      ray -> rmax = mod -> oargs.farg[3];
   else
      ray -> rmax = photonMaxDist;
   rayorigin(ray, PRIMARY, NULL, NULL);
   
   if (!emap -> numSamples) {
      /* Source is unmodified and has no port, and either local with 
         normal aligned aperture, or distant with aperture above surface; 
         use cosine weighted distribution */
      cosThetaSqr = 1 - pmapRandom(emitState) * 
                        (1 - sqr(max(emap -> cosThetaMax, 0)));
      cosTheta = sqrt(cosThetaSqr);
      sinTheta = sqrt(1 - cosThetaSqr);
      phi = 2 * PI * pmapRandom(emitState);
      setcolor(ray -> rcol, mod -> oargs.farg [0], mod -> oargs.farg [1], 
               mod -> oargs.farg [2]);
   }
   
   else {
      /* Source is either modified, has a port, is local with off-normal 
         aperture, or distant with aperture partly below surface; choose 
         direction from constructed cumulative distribution function with
         Monte Carlo inversion using binary search. */
      du = pmapRandom(emitState) * emap -> cdf;
      lo = 1;
      hi = emap -> numSamples;
      
      while (hi > lo) {
         i = (lo + hi) >> 1;
         sample = emap -> samples + i - 1;
         
         if (sample -> cdf >= du) 
            hi = i;
         if (sample -> cdf < du) 
            lo = i + 1;
      }
      
      /* This is a uniform mapping, mon */
      cosTheta = 1 - (sample -> theta + pmapRandom(emitState)) *
                     (1 - emap -> cosThetaMax) / emap -> numTheta;
      sinTheta = sqrt(1 - sqr(cosTheta));
      phi = 2 * PI * (sample -> phi + pmapRandom(emitState)) / emap -> numPhi;
      copycolor(ray -> rcol, sample -> pdf);
   }

   /* Normalize photon flux so that average over RGB is 1 */
   colorNorm(ray -> rcol);   
   
   VCOPY(ray -> rorg, emap -> photonOrg);   
   du = cos(phi) * sinTheta;
   dv = sin(phi) * sinTheta;
   
   for (i = 0; i < 3; i++)
      ray -> rdir [i] = du * emap -> uh [i] + dv * emap -> vh [i] + 
                        cosTheta * emap -> wh [i];
                        
   if (emap -> src -> sflags & SDISTANT)
      /* Distant source; reverse ray direction to point into the scene. */     
      for (i = 0; i < 3; i++) 
         ray -> rdir [i] = -ray -> rdir [i];
      
   if (emap -> port)
      /* Photon emitted from port; move origin just behind port so it 
         will be scattered */
      for (i = 0; i < 3; i++) 
         ray -> rorg [i] -= 2 * FTINY * ray -> rdir [i];    
   
   /* Assign emitting light source index */
   ray -> rsrc = emap -> src - source;
}



void multDirectPmap (RAY *r)
/* Factor irradiance from direct photons into r -> rcol; interface to
 * direct() */
{
   COLOR photonIrrad;
   
   /* Lookup direct photon irradiance */
   (directPmap -> lookup)(directPmap, r, photonIrrad);
   
   /* Multiply with coefficient in ray */
   multcolor(r -> rcol, photonIrrad);
   
   return;
}



void inscatterVolumePmap (RAY *r, COLOR inscatter)
/* Add inscattering from volume photon map; interface to srcscatter() */
{
   /* Add ambient in-scattering via lookup callback */
   (volumePmap -> lookup)(volumePmap, r, inscatter);
}
