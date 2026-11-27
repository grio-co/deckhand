// deckhand-plugin-probe <plugin id> [sample rate] [block]: opens one plug-in, renders one block, prints one result line.
#include "deckhand/plugins/probe.hpp"
int main(int argc, char** argv) { return deckhand::plugins::probe_main(argc, argv); }
