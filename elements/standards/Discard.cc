#include "Discard.hh"
#include "../../lib/types.hh"

using namespace std;
using namespace nba;

int Discard::initialize()
{
    return 0;
}

int Discard::configure(comp_thread_context *ctx, std::vector<std::string> &args)
{
    Element::configure(ctx, args);
    return 0;
}

int Discard::process(int input_port, Packet *pkt)
{
    return DROP;
}

// vim: ts=8 sts=4 sw=4 et
