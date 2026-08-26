// The six statement lines below are repeated verbatim in dup_two.cc. The
// signature differs so that exactly one sliding window is shared, which is what
// the fixture asserts; a seventh shared line would report two findings.
void PlantedDupOne() {
  int alpha_value = 1;
  int beta_value = alpha_value + 2;
  int gamma_value = beta_value + 3;
  int delta_value = gamma_value + 4;
  int epsilon_value = delta_value + 5;
  int zeta_value = epsilon_value + 6;
}
