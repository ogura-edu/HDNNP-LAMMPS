/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Aidan Thompson (SNL)
------------------------------------------------------------------------- */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include "atom.h"
#include "comm.h"
#include "error.h"
#include "force.h"
#include "memory.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "neighbor.h"
#include "pair_nnp.h"

using namespace LAMMPS_NS;

#define MAXLINE 1024
#define DELTA 4

/* ---------------------------------------------------------------------- */

PairNNP::PairNNP(LAMMPS *lmp) : Pair(lmp) {
  single_enable = 0;
  restartinfo = 0;
  one_coeff = 1;
  manybody_flag = 1;

  nelements = 0;
  nG1params = nG2params = nG4params = 0;
}

/* ----------------------------------------------------------------------
   check if allocated, since class can be destructed when incomplete
------------------------------------------------------------------------- */

PairNNP::~PairNNP() {
  int i, j;
  if (copymode) return;

  if (allocated) {
    memory->destroy(cutsq);
    memory->destroy(setflag);
  }
}

/* ---------------------------------------------------------------------- */

void PairNNP::compute(int eflag, int vflag) {
  int i, j, ii, jj, inum, jnum, p;
  int itype, jtype, iparam;
  double evdwl, fx, fy, fz, delx, dely, delz;
  int *ilist, *jlist, *numneigh, **firstneigh;
  vector<int> iG2s;
  vector<vector<int> > iG3s;
  VectorXd r[3], R, dR[3];
  MatrixXd cos, dcos[3];
  VectorXd G, dE_dG, F[3];
  MatrixXd dG_dx, dG_dy, dG_dz;

  evdwl = 0.0;
  if (eflag || vflag)
    ev_setup(eflag, vflag);
  else
    evflag = vflag_fdotr = 0;

  double **x = atom->x;
  double **f = atom->f;
  int *type = atom->type;

  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];          // local index of I atom
    itype = map[type[i]];   // element
    jlist = firstneigh[i];  // indices of J neighbors of I atom
    jnum = numneigh[i];     // # of J neighbors of I atom

    geometry(i, jlist, jnum, r, R, cos, dR, dcos);

    G = VectorXd::Zero(nfeature);
    dG_dx = MatrixXd::Zero(nfeature, jnum);
    dG_dy = MatrixXd::Zero(nfeature, jnum);
    dG_dz = MatrixXd::Zero(nfeature, jnum);

    feature_index(jlist, jnum, iG2s, iG3s);
    for (iparam = 0; iparam < nG1params; iparam++)
      G1(G1params[iparam], ntwobody * iparam, iG2s, jnum, R, dR, G,
         dG_dx, dG_dy, dG_dz);
    for (iparam = 0; iparam < nG2params; iparam++)
      G2(G2params[iparam], ntwobody * (nG1params + iparam), iG2s, jnum, R, dR,
         G, dG_dx, dG_dy, dG_dz);
    for (iparam = 0; iparam < nG4params; iparam++)
      G4(G4params[iparam],
         ntwobody * (nG1params + nG2params) + nthreebody * iparam, iG3s, jnum,
         R, cos, dR, dcos, G, dG_dx, dG_dy, dG_dz);

    for (p = 0; p < npreprocess; p++) {
      (this->*preprocesses[p])(itype, G, dG_dx, dG_dy, dG_dz);
    }

    masters[itype].feedforward(G, dE_dG, eflag, evdwl);
    evdwl *= 2.0 / jnum;

    F[0].noalias() = -1.0 * dE_dG.transpose() * dG_dx;
    F[1].noalias() = -1.0 * dE_dG.transpose() * dG_dy;
    F[2].noalias() = -1.0 * dE_dG.transpose() * dG_dz;

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      fx = F[0].coeffRef(jj);
      fy = F[1].coeffRef(jj);
      fz = F[2].coeffRef(jj);
      delx = r[0].coeffRef(jj);
      dely = r[1].coeffRef(jj);
      delz = r[2].coeffRef(jj);
      f[j][0] += fx;
      f[j][1] += fy;
      f[j][2] += fz;

      if (evflag)
        ev_tally_xyz_full(i, evdwl, 0.0, fx, fy, fz, delx, dely, delz);
    }
  }

  if (vflag_fdotr) virial_fdotr_compute();
}

/* ---------------------------------------------------------------------- */

void PairNNP::allocate() {
  allocated = 1;
  int n = atom->ntypes;

  memory->create(cutsq, n + 1, n + 1, "pair:cutsq");
  memory->create(setflag, n + 1, n + 1, "pair:setflag");
  map = vector<int>(n + 1);
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairNNP::settings(int narg, char **arg) {
  if (narg != 0) error->all(FLERR, "Illegal pair_style command");
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairNNP::coeff(int narg, char **arg) {
  int i, j, idx;
  int ntypes = atom->ntypes;

  if (!allocated) allocate();

  if (narg != 3 + ntypes)
    error->all(FLERR, "Incorrect args for pair coefficients");

  // insure I,J args are * *

  if (strcmp(arg[0], "*") != 0 || strcmp(arg[1], "*") != 0)
    error->all(FLERR, "Incorrect args for pair coefficients");

  // read args that map atom types to elements in potential file
  // map[i] = which element the Ith atom type is, -1 if NULL
  // nelements = # of unique elements
  // elements = list of element names

  if (!elements.empty()) elements.clear();

  nelements = 0;
  for (i = 3; i < narg; i++) {
    if (strcmp(arg[i], "NULL") == 0) {
      map[i - 2] = -1;
      continue;
    }
    for (j = 0; j < nelements; j++)
      if (string(arg[i]) == elements[j]) break;
    map[i - 2] = j;
    if (j == nelements) {
      elements.push_back(string(arg[i]));
      nelements++;
    }
  }
  combinations = vector<vector<int> >(ntypes, vector<int>(ntypes));
  idx = 0;
  for (i = 0; i < nelements; i++)
    for (j = i; j < nelements; j++)
      combinations[i][j] = combinations[j][i] = idx++;
  ntwobody = nelements;
  nthreebody = idx;

  // read potential file and initialize potential parameters

  read_file(arg[2]);
  setup_params();

  cutmax = 0.0;
  for (i = 0; i < nG1params; i++)
    if (G1params[i][0] > cutmax) cutmax = G1params[i][0];
  for (i = 0; i < nG2params; i++)
    if (G2params[i][0] > cutmax) cutmax = G2params[i][0];
  for (i = 0; i < nG4params; i++)
    if (G4params[i][0] > cutmax) cutmax = G4params[i][0];

  for (i = 1; i < ntypes + 1; i++) {
    for (j = 1; j < ntypes + 1; j++) {
      cutsq[i][j] = cutmax * cutmax;
      setflag[i][j] = 1;
    }
  }
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairNNP::init_style() {
  if (force->newton_pair == 0)
    error->all(FLERR,
               "Pair style Neural Network Potential requires newton pair on");
  // need a full neighbor list

  int irequest = neighbor->request(this, instance_me);
  neighbor->requests[irequest]->half = 0;
  neighbor->requests[irequest]->full = 1;
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairNNP::init_one(int i, int j) {
  if (setflag[i][j] == 0) error->all(FLERR, "All pair coeffs are not set");

  return cutmax;
}

/* ---------------------------------------------------------------------- */

void PairNNP::get_next_line(ifstream &fin, stringstream &ss, int &nwords) {
  string line;
  int n;

  // remove failbit
  ss.clear();
  // clear stringstream buffer
  ss.str("");

  if (comm->me == 0)
    while (getline(fin, line))
      if (!line.empty() && line[0] != '#') break;

  n = line.size();
  MPI_Bcast(&n, 1, MPI_INT, 0, world);
  line.resize(n);

  MPI_Bcast(&line[0], n + 1, MPI_CHAR, 0, world);
  nwords = atom->count_words(line.c_str());
  ss << line;
}

void PairNNP::read_file(char *file) {
  ifstream fin;
  stringstream ss;
  string sym_func_type, preprocess, element, activation;
  int i, j, k, l, nwords;
  int ntype, depth, depthnum, insize, outsize, size;
  double Rc, eta, Rs, lambda, zeta;
  vector<double> pca_transform_raw, pca_mean_raw;
  vector<double> scl_max_raw, scl_min_raw;
  vector<double> std_mean_raw, std_std_raw;
  vector<double> weight, bias;

  if (comm->me == 0) {
    fin.open(file);
    if (!fin) {
      char str[128];
      sprintf(str, "Cannot open neural network potential file %s", file);
      error->one(FLERR, str);
    }
  }

  // symmetry function parameters
  nG1params = 0;
  nG2params = 0;
  nG4params = 0;
  get_next_line(fin, ss, nwords);
  ss >> ntype;

  for (i = 0; i < ntype; i++) {
    get_next_line(fin, ss, nwords);
    ss >> sym_func_type >> size;
    if (sym_func_type == "type1") {
      nG1params = size;
      G1params = vector<vector<double> >(nG1params);
      for (j = 0; j < nG1params; j++) {
        get_next_line(fin, ss, nwords);
        ss >> Rc;
        G1params[j].push_back(Rc);
      }
    } else if (sym_func_type == "type2") {
      nG2params = size;
      G2params = vector<vector<double> >(nG2params);
      for (j = 0; j < nG2params; j++) {
        get_next_line(fin, ss, nwords);
        ss >> Rc >> eta >> Rs;
        G2params[j].push_back(Rc);
        G2params[j].push_back(eta);
        G2params[j].push_back(Rs);
      }
    } else if (sym_func_type == "type4") {
      nG4params = size;
      G4params = vector<vector<double> >(nG4params);
      for (j = 0; j < nG4params; j++) {
        get_next_line(fin, ss, nwords);
        ss >> Rc >> eta >> lambda >> zeta;
        G4params[j].push_back(Rc);
        G4params[j].push_back(eta);
        G4params[j].push_back(lambda);
        G4params[j].push_back(zeta);
      }
    }
  }
  nfeature = ntwobody * (nG1params + nG2params) + nthreebody * nG4params;

  // preprocess parameters
  get_next_line(fin, ss, nwords);
  ss >> npreprocess;

  for (i = 0; i < npreprocess; i++) {
    get_next_line(fin, ss, nwords);
    ss >> preprocess;

    if (preprocess == "pca") {
      preprocesses.push_back(&PairNNP::pca);
      pca_transform = vector<MatrixXd>(nelements);
      pca_mean = vector<VectorXd>(nelements);
      for (j = 0; j < nelements; j++) {
        get_next_line(fin, ss, nwords);
        ss >> element >> outsize >> insize;
        pca_transform_raw = vector<double>(insize * outsize);
        pca_mean_raw = vector<double>(insize);

        for (k = 0; k < outsize; k++) {
          get_next_line(fin, ss, nwords);
          for (l = 0; ss >> pca_transform_raw[k * insize + l]; l++)
            ;
        }

        get_next_line(fin, ss, nwords);
        for (k = 0; ss >> pca_mean_raw[k]; k++)
          ;

        for (k = 0; k < nelements; k++)
          if (elements[k] == element) {
            pca_transform[k] =
                Map<MatrixXd>(&pca_transform_raw[0], insize, outsize).transpose();
            pca_mean[k] = Map<VectorXd>(&pca_mean_raw[0], insize);
          }
      }
    } else if (preprocess == "scaling") {
      preprocesses.push_back(&PairNNP::scaling);
      scl_max = vector<VectorXd>(nelements);
      scl_min = vector<VectorXd>(nelements);

      get_next_line(fin, ss, nwords);
      ss >> scl_target_max >> scl_target_min;

      for (j = 0; j < nelements; j++) {
        get_next_line(fin, ss, nwords);
        ss >> element >> size;
        scl_max_raw = vector<double>(size);
        scl_min_raw = vector<double>(size);

        get_next_line(fin, ss, nwords);
        for (k = 0; ss >> scl_max_raw[k]; k++)
          ;

        get_next_line(fin, ss, nwords);
        for (k = 0; ss >> scl_min_raw[k]; k++)
          ;

        for (k = 0; k < nelements; k++)
          if (elements[k] == element) {
            scl_max[k] = Map<VectorXd>(&scl_max_raw[0], size);
            scl_min[k] = Map<VectorXd>(&scl_min_raw[0], size);
          }
      }
    } else if (preprocess == "standardization") {
      preprocesses.push_back(&PairNNP::standardization);
      std_mean = vector<VectorXd>(nelements);
      std_std = vector<VectorXd>(nelements);

      for (j = 0; j < nelements; j++) {
        get_next_line(fin, ss, nwords);
        ss >> element >> size;
        std_mean_raw = vector<double>(size);
        std_std_raw = vector<double>(size);

        get_next_line(fin, ss, nwords);
        for (k = 0; ss >> std_mean_raw[k]; k++)
          ;

        get_next_line(fin, ss, nwords);
        for (k = 0; ss >> std_std_raw[k]; k++)
          ;

        for (k = 0; k < nelements; k++)
          if (elements[k] == element) {
            std_mean[k] = Map<VectorXd>(&std_mean_raw[0], size);
            std_std[k] = Map<VectorXd>(&std_std_raw[0], size);
          }
      }
    }
  }

  // neural network parameters
  get_next_line(fin, ss, nwords);
  ss >> depth;
  for (i = 0; i < nelements; i++) masters.push_back(NNP(depth));

  for (i = 0; i < nelements * depth; i++) {
    get_next_line(fin, ss, nwords);
    ss >> element >> depthnum >> insize >> outsize >> activation;
    weight = vector<double>(insize * outsize);
    bias = vector<double>(outsize);

    for (j = 0; j < insize; j++) {
      get_next_line(fin, ss, nwords);
      for (k = 0; ss >> weight[j * outsize + k]; k++)
        ;
    }

    get_next_line(fin, ss, nwords);
    for (j = 0; ss >> bias[j]; j++)
      ;

    for (j = 0; j < nelements; j++)
      if (elements[j] == element)
        masters[j].layers.push_back(Layer(insize, outsize, weight, bias, activation));
  }
  if (comm->me == 0) fin.close();
}

/* ---------------------------------------------------------------------- */

void PairNNP::setup_params() {}

/* ---------------------------------------------------------------------- */

void PairNNP::geometry(int i, int *jlist, int jnum, VectorXd *r, VectorXd &R,
                       MatrixXd &cos, VectorXd *dR, MatrixXd *dcos) {
  int jj, j;
  double **x = atom->x;
  MatrixXd r_, dR_;

  r_ = MatrixXd(jnum, 3);
  for (jj = 0; jj < jnum; jj++) {
    j = jlist[jj];
    r_.coeffRef(jj, 0) = x[j][0] - x[i][0];
    r_.coeffRef(jj, 1) = x[j][1] - x[i][1];
    r_.coeffRef(jj, 2) = x[j][2] - x[i][2];
  }

  R = r_.rowwise().norm();
  dR_ = r_.array().colwise() / R.array();
  cos.noalias() = dR_ * dR_.transpose();
  for (i = 0; i < 3; i++) {
    r[i] = r_.col(i);
    dR[i] = dR_.col(i);
    dcos[i] = ((cos.array().colwise() * dR[i].array() * (-1.0)).rowwise()
               + dR[i].transpose().array()
              ).colwise() * R.array().inverse();
  }
}

void PairNNP::feature_index(int *jlist, int jnum, std::vector<int> &iG2s,
                            vector< vector<int> > &iG3s) {
  int i, j, itype, jtype;
  int *type = atom->type;
  iG2s = vector<int>(jnum);
  iG3s = vector<vector<int> >(jnum, vector<int>(jnum));
  for (i = 0; i < jnum; i++) {
    itype = map[type[jlist[i]]];
    iG2s[i] = itype;

    for (j = 0; j < jnum; j++) {
      jtype = map[type[jlist[j]]];
      iG3s[i][j] = combinations[itype][jtype];
    }
  }
}

void PairNNP::pca(int type, VectorXd &G, MatrixXd &dG_dx, MatrixXd &dG_dy,
                  MatrixXd &dG_dz) {
  G = pca_transform[type] * (G - pca_mean[type]);
  dG_dx = pca_transform[type] * dG_dx;
  dG_dy = pca_transform[type] * dG_dy;
  dG_dz = pca_transform[type] * dG_dz;
}

void PairNNP::scaling(int type, VectorXd &G, MatrixXd &dG_dx, MatrixXd &dG_dy,
                      MatrixXd &dG_dz) {
  G = ((G - scl_min[type]).array() *
       (scl_max[type] - scl_min[type]).array().inverse() *
       (scl_target_max - scl_target_min))
          .array() +
      scl_target_min;
  dG_dx = dG_dx.array().colwise() *
          (scl_max[type] - scl_min[type]).array().inverse() *
          (scl_target_max - scl_target_min);
  dG_dy = dG_dy.array().colwise() *
          (scl_max[type] - scl_min[type]).array().inverse() *
          (scl_target_max - scl_target_min);
  dG_dz = dG_dz.array().colwise() *
          (scl_max[type] - scl_min[type]).array().inverse() *
          (scl_target_max - scl_target_min);
}

void PairNNP::standardization(int type, VectorXd &G, MatrixXd &dG_dx,
                              MatrixXd &dG_dy, MatrixXd &dG_dz) {
  G = (G - std_mean[type]).array() * std_std[type].array().inverse();
  dG_dx = dG_dx.array().colwise() * std_std[type].array().inverse();
  dG_dy = dG_dy.array().colwise() * std_std[type].array().inverse();
  dG_dz = dG_dz.array().colwise() * std_std[type].array().inverse();
}
