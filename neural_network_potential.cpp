#include "neural_network_potential.h"

Layer::Layer(int in, int out, double *w, double *b, string act) {
  weight = Map<MatrixXd>(&w[0], out, in);
  bias = Map<VectorXd>(&b[0], out);
  set_activation(act);
}

Layer::~Layer() {}

void Layer::tanh(VectorXd &input) { input = input.array().tanh(); }

void Layer::deriv_tanh(VectorXd &input, VectorXd &deriv) {
  input = input.array().tanh();
  // (tanh)' = 1 - tanh^2
  deriv = 1.0 - input.array().square();
}

void Layer::elu(VectorXd &input) {
  input = (input.array() > 0).select(input, input.array().exp() - 1.0);
}

void Layer::deriv_elu(VectorXd &input, VectorXd &deriv) {
  // (elu)' = 1 or exp (border:x=0)
  deriv = (input.array() > 0)
              .select(VectorXd::Ones(input.size()), input.array().exp());
  // elu = x or exp-1 (border:x=0)
  input = (input.array() > 0).select(input, input.array().exp() - 1.0);
}

void Layer::sigmoid(VectorXd &input) {
  input = 1.0 / (1.0 + (-input).array().exp());
}

void Layer::deriv_sigmoid(VectorXd &input, VectorXd &deriv) {
  input = 1.0 / (1.0 + (-input).array().exp());
  // (sigmoid)' = sigmoid * (1-sigmoid)
  deriv = input.array() * (1.0 - input.array());
}

void Layer::identity(VectorXd &input) {}

void Layer::deriv_identity(VectorXd &input, VectorXd &deriv) {
  deriv = VectorXd::Ones(input.size());
}

void Layer::set_activation(string act) {
  if (act == "tanh") {
    activation = &Layer::tanh;
    activation2 = &Layer::deriv_tanh;
  } else if (act == "elu") {
    activation = &Layer::elu;
    activation2 = &Layer::deriv_elu;
  } else if (act == "sigmoid") {
    activation = &Layer::sigmoid;
    activation2 = &Layer::deriv_sigmoid;
  } else if (act == "identity") {
    activation = &Layer::identity;
    activation2 = &Layer::deriv_identity;
  } else {
    cout << "ERROR!! not implemented ACTIVATION FUNCTION!!" << endl;
  }
}

void Layer::feedforward(VectorXd &input) {
  input = (weight * input).colwise() + bias;
  (this->*activation)(input);
}

void Layer::feedforward2(VectorXd &input, VectorXd &deriv) {
  input = (weight * input).colwise() + bias;
  (this->*activation2)(input, deriv);
}

NNP::NNP(int n) {
  depth = n;
  layers = new Layer *[depth];
}

NNP::~NNP() { delete[] layers; }

void NNP::energy(VectorXd input, double &E) {
  int i;
  for (i = 0; i < depth; i++) layers[i]->feedforward(input);
  E += input(0);
}

void NNP::deriv(VectorXd input, VectorXd &dE_dG) {
  int i;
  VectorXd deriv[depth];

  for (i = 0; i < depth; i++) layers[i]->feedforward2(input, deriv[i]);
  dE_dG = VectorXd::Ones(1);
  for (i = depth - 1; i >= 0; i--) {
    dE_dG = dE_dG.array() * deriv[i].array();
    dE_dG = dE_dG.transpose() * layers[i]->weight;
  }
}
