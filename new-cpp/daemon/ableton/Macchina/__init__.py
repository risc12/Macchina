# Macchina — Ableton Live Remote Script (Live 11/12)
#
# A thin OSC/UDP client of macchinad (which owns the Maschine Studio).
# Install: copy this folder to
#   ~/Music/Ableton/User Library/Remote Scripts/Macchina
# then pick "Macchina" as a Control Surface in Live > Settings > Link/MIDI
# (leave input/output at "None" — everything goes over the socket).

from .Macchina import Macchina


def create_instance(c_instance):
    return Macchina(c_instance)
