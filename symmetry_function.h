//
// Created by Masayoshi Ogura on 2018/06/29.
//

#ifndef HDNNP_LAMMPS_SYMMETRY_FUNCTION_H
#define HDNNP_LAMMPS_SYMMETRY_FUNCTION_H
#define INCLUDED_NNP_H_
#define EIGEN_USE_MKL_ALL
#define EIGEN_NO_DEBUG
#define EIGEN_MPL2_ONLY

#include <Eigen/Core>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace Eigen;

void G1(vector<double>, int, vector<int>, int, VectorXd &, VectorXd *,
        VectorXd &, MatrixXd &, MatrixXd &, MatrixXd &);

void G2(vector<double>, int, vector<int>, int, VectorXd &, VectorXd *,
        VectorXd &, MatrixXd &, MatrixXd &, MatrixXd &);

void G4(vector<double>, int, vector<vector<int> >, int, VectorXd &, MatrixXd &,
        VectorXd *, MatrixXd *, VectorXd &, MatrixXd &, MatrixXd &, MatrixXd &);

#endif  // HDNNP_LAMMPS_SYMMETRY_FUNCTION_H
