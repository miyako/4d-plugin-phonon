//%attributes = {"invisible":true,"preemptive":"capable"}
var $model : Integer
$model:=Phonon Load model(phonon_test_model.path; {})

var $speech : Integer
$speech:=Phonon Play($model; "This sentence is long enough to be paused and resumed while it is playing."; {output: Phonon output none})
ASSERT($speech>0; Phonon Get last error)

var $status : Object
var $deadline : Real
$deadline:=Milliseconds+30000
Repeat 
	DELAY PROCESS(Current process; 2)
	$status:=Phonon Get status($speech)
Until (($status.position>0.5) || ($status.state=Phonon state finished) || ($status.state=Phonon state error) || (Milliseconds>$deadline))
ASSERT($status.state=Phonon state playing; JSON Stringify($status))

Phonon PAUSE($speech)
ASSERT(Phonon Get status($speech).state=Phonon state paused)
DELAY PROCESS(Current process; 3)
var $position : Real
$position:=Phonon Get status($speech).position
DELAY PROCESS(Current process; 30)
ASSERT(Phonon Get status($speech).position=$position)
ASSERT(Phonon Get status($speech).state=Phonon state paused)

Phonon RESUME($speech)
ASSERT(Phonon Get status($speech).state=Phonon state playing)
DELAY PROCESS(Current process; 20)
ASSERT(Phonon Get status($speech).position>$position)

// all at once
Phonon PAUSE ALL
ASSERT(Phonon Get status($speech).state=Phonon state paused)
Phonon RESUME ALL
ASSERT(Phonon Get status($speech).state=Phonon state playing)

Phonon SET VOLUME($speech; 0.5)
ASSERT(Phonon Get status($speech).volume=0.5; JSON Stringify(Phonon Get status($speech))+Phonon Get last error)
Phonon SET VOLUME($speech; -1)
ASSERT(Phonon Get last error#"")

// stop in the middle
Phonon STOP($speech)
ASSERT(Phonon Get status($speech).state=Phonon state unknown)

// a speech can start paused
$speech:=Phonon Play($model; "Wait for it."; {output: Phonon output none; paused: True})
DELAY PROCESS(Current process; 30)
$status:=Phonon Get status($speech)
ASSERT($status.state=Phonon state paused)
ASSERT($status.position=0)
Phonon RESUME($speech)
ASSERT(phonon_test_wait($speech; 60).state=Phonon state finished)
Phonon STOP($speech)

Phonon UNLOAD MODEL($model)
