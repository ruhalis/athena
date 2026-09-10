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

  Voice station (on the Mac mini, talks through the ESP32 audio board)

  ssh delphi@100.95.128.31
  /opt/homebrew/bin/python3 ~/athena-face/scripts/station.py      # hears the board's mic, answers on its speaker; Ctrl-C stops it

  Useful variants:

  ... station.py --question "сколько объектов с нарушениями"        # no mic: ask this, speak the answer, exit
  ... station.py --inject question.wav --once                        # a WAV as the question, then exit
  ... station.py --no-speak --no-face --question "..."               # brain only, prints what it would say
  ... station.py --gain 12                                           # louder (clips harder); default 6
  ... station.py --stt whispercpp                                    # whisper.cpp on 8088 instead of the gateway's whisper (4x faster)
  ... station.py --vad-db 14                                         # a noisier room: voice must be further above the floor

  It never listens while it thinks or speaks: the mic opens again ~0.6 s after the last word.
  Update it: on the laptop commit to main, copy to the face-plugin branch (see SETUP.md), on the mini git -C ~/athena-face pull.

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

 ./scripts/audio.py play fullpower.wav --gain 20
 say -o hi.wav --data-format=LEI16@16000 "Hello from Athena" && ./scripts/audio.py play hi.wav
 ./scripts/audio.py play one_small_step_16k.wav 

 ./scripts/audio.py record 5 take.wav
 ./scripts/audio.py play take.wav