Both live in ~/projects/athena/scripts/. The voice bridge is still running in the background right
  now, so stop it first if you want to start it in the foreground.

  Voice loop (talk to Athena)

  cd ~/projects/athena
  ./scripts/voice.py --stop        # stop the copy I left running
  ./scripts/voice.py               # start in the foreground, Ctrl-C to stop

  Useful variants:

  ./scripts/voice.py --voice cedar --vad semantic   # different voice, smarter turn-taking
  ./scripts/voice.py --barge-in                     # with headphones: interrupt her mid-sentence
  ./scripts/voice.py --no-face                      # without the matrix
  ./scripts/voice.py --list-devices                 # pick a mic/speaker with --input / --output

  To run it in the background like now:

  nohup ./scripts/voice.py > ~/.hermes/logs/athena-voice.log 2>&1 &
  tail -f ~/.hermes/logs/athena-voice.log

  Matrix demo (walk through every state)

  cd ~/projects/athena
  ./scripts/face.py --demo                 # idle → listen → think → work → speak → alert → error →
  sleep → idle, 4 s each
  ./scripts/face.py --demo --pause 2       # faster
  ./scripts/face.py --host athena-matrix.local --demo   # force Wi-Fi if a board is on USB

  Single states and settings:

  ./scripts/face.py think
  ./scripts/face.py alert --t "HI"
  ./scripts/face.py --brightness 60
  ./scripts/face.py --ping                 # expect "ok"
  ./scripts/face.py idle                   # back to the clock

  The demo can run while the voice bridge is up. The board accepts several Wi-Fi connections, and
  the bridge will overwrite the demo's state on the next conversation turn.
