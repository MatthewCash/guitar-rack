declare id     "dcblocker";
declare name   "dcblocker";
declare category "Tone Control";

import("stdfaust.lib");

process = fi.dcblockerat(1.0), fi.dcblockerat(1.0);
