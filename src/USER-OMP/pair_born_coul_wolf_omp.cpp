/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   This software is distributed under the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Axel Kohlmeyer (Temple U)
------------------------------------------------------------------------- */

#include "omp_compat.h"
#include "pair_born_coul_wolf_omp.h"
#include <cmath>
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "neigh_list.h"
#include "math_const.h"


#include "suffix.h"
using namespace LAMMPS_NS;
using namespace MathConst;

/* ---------------------------------------------------------------------- */

PairBornCoulWolfOMP::PairBornCoulWolfOMP(LAMMPS *lmp) :
  PairBornCoulWolf(lmp), ThrOMP(lmp, THR_PAIR)
{
  suffix_flag |= Suffix::OMP;
  respa_enable = 0;
}

/* ---------------------------------------------------------------------- */

void PairBornCoulWolfOMP::compute(int eflag, int vflag)
{
  ev_init(eflag,vflag);

  const int nall = atom->nlocal + atom->nghost;
  const int nthreads = comm->nthreads;
  const int inum = list->inum;

#if defined(_OPENMP)
#pragma omp parallel LMP_DEFAULT_NONE LMP_SHARED(eflag,vflag)
#endif
  {
    int ifrom, ito, tid;

    loop_setup_thr(ifrom, ito, tid, inum, nthreads);
    ThrData *thr = fix->get_thr(tid);
    thr->timer(Timer::START);
    ev_setup_thr(eflag, vflag, nall, eatom, vatom, nullptr, thr);

    if (evflag) {
      if (eflag) {
        if (force->newton_pair) eval<1,1,1>(ifrom, ito, thr);
        else eval<1,1,0>(ifrom, ito, thr);
      } else {
        if (force->newton_pair) eval<1,0,1>(ifrom, ito, thr);
        else eval<1,0,0>(ifrom, ito, thr);
      }
    } else {
      if (force->newton_pair) eval<0,0,1>(ifrom, ito, thr);
      else eval<0,0,0>(ifrom, ito, thr);
    }

    thr->timer(Timer::PAIR);
    reduce_thr(this, eflag, vflag, thr);
  } // end of omp parallel region
}

/* ---------------------------------------------------------------------- */

template <int EVFLAG, int EFLAG, int NEWTON_PAIR>
void PairBornCoulWolfOMP::eval(int iifrom, int iito, ThrData * const thr)
{
  int i,j,ii,jj,jnum,itype,jtype;
  double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,evdwl,ecoul,fpair;
  double rsq,r2inv,r6inv,forcecoul,forceborn,factor_coul,factor_lj;
  double prefactor;
  double r,rexp;
  int *ilist,*jlist,*numneigh,**firstneigh;
  double erfcc,erfcd,v_sh,dvdrr,e_self,e_shift,f_shift,qisq;

  evdwl = ecoul = 0.0;

  const dbl3_t * _noalias const x = (dbl3_t *) atom->x[0];
  dbl3_t * _noalias const f = (dbl3_t *) thr->get_f()[0];
  const double * _noalias const q = atom->q;
  const int * _noalias const type = atom->type;
  int nlocal = atom->nlocal;
  double *special_coul = force->special_coul;
  double *special_lj = force->special_lj;
  double qqrd2e = force->qqrd2e;
  double fxtmp,fytmp,fztmp;

  // self and shifted coulombic energy

  e_self = v_sh = 0.0;
  e_shift = erfc(alf*cut_coul)/cut_coul;
  f_shift = -(e_shift+ 2.0*alf/MY_PIS * exp(-alf*alf*cut_coul*cut_coul)) /
    cut_coul;

  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  // loop over neighbors of my atoms

  for (ii = iifrom; ii < iito; ++ii) {

    i = ilist[ii];
    qtmp = q[i];
    xtmp = x[i].x;
    ytmp = x[i].y;
    ztmp = x[i].z;
    itype = type[i];
    jlist = firstneigh[i];
    jnum = numneigh[i];
    fxtmp=fytmp=fztmp=0.0;

    qisq = qtmp*qtmp;
    e_self = -(e_shift/2.0 + alf/MY_PIS) * qisq*qqrd2e;
    if (EFLAG) ev_tally_thr(this,i,i,nlocal,0,0.0,e_self,0.0,0.0,0.0,0.0,thr);

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      factor_lj = special_lj[sbmask(j)];
      factor_coul = special_coul[sbmask(j)];
      j &= NEIGHMASK;

      delx = xtmp - x[j].x;
      dely = ytmp - x[j].y;
      delz = ztmp - x[j].z;
      rsq = delx*delx + dely*dely + delz*delz;
      jtype = type[j];

      if (rsq < cutsq[itype][jtype]) {
        r2inv = 1.0/rsq;
        r = sqrt(rsq);

        if (rsq < cut_coulsq) {
          prefactor = qqrd2e*qtmp*q[j]/r;
          erfcc = erfc(alf*r);
          erfcd = exp(-alf*alf*r*r);
          v_sh = (erfcc - e_shift*r) * prefactor;
          dvdrr = (erfcc/rsq + 2.0*alf/MY_PIS * erfcd/r) + f_shift;
          forcecoul = dvdrr*rsq*prefactor;
          if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*prefactor;
        } else forcecoul = 0.0;

        if (rsq < cut_ljsq[itype][jtype]) {
          r6inv = r2inv*r2inv*r2inv;
          rexp = exp((sigma[itype][jtype]-r)*rhoinv[itype][jtype]);
          forceborn = born1[itype][jtype]*r*rexp - born2[itype][jtype]*r6inv
            + born3[itype][jtype]*r2inv*r6inv;
        } else forceborn = 0.0;

        fpair = (forcecoul + factor_lj*forceborn)*r2inv;

        fxtmp += delx*fpair;
        fytmp += dely*fpair;
        fztmp += delz*fpair;
        if (NEWTON_PAIR || j < nlocal) {
          f[j].x -= delx*fpair;
          f[j].y -= dely*fpair;
          f[j].z -= delz*fpair;
        }

        if (EFLAG) {
          if (rsq < cut_coulsq) {
            ecoul = v_sh;
            if (factor_coul < 1.0) ecoul -= (1.0-factor_coul)*prefactor;
          } else ecoul = 0.0;
          if (rsq < cut_ljsq[itype][jtype]) {
            evdwl = a[itype][jtype]*rexp - c[itype][jtype]*r6inv +
              d[itype][jtype]*r6inv*r2inv - offset[itype][jtype];
            evdwl *= factor_lj;
          } else evdwl = 0.0;
        }

        if (EVFLAG) ev_tally_thr(this,i,j,nlocal,NEWTON_PAIR,
                                 evdwl,ecoul,fpair,delx,dely,delz,thr);
      }
    }
    f[i].x += fxtmp;
    f[i].y += fytmp;
    f[i].z += fztmp;
  }
}

/* ---------------------------------------------------------------------- */

double PairBornCoulWolfOMP::memory_usage()
{
  double bytes = memory_usage_thr();
  bytes += PairBornCoulWolf::memory_usage();

  return bytes;
}
