//%attributes = {"invisible":true,"preemptive":"capable"}
var $path : Text
$path:=phonon_test_model.path

var $models : Collection
$models:=Phonon Load models([$path; $path])

// in sequence: each speech plays after the one before it
var $refs : Collection
$refs:=Phonon Play multiple(["First sentence."; {text: "Second sentence."; volume: 0.8}; {text: "Third, from the other model."; model: $models[1]}]; {model: $models[0]; sequential: True; output: Phonon output none})
ASSERT($refs.length=3)
ASSERT($refs.indexOf(0)=-1; Phonon Get last error)

var $statuses : Collection
$statuses:=Phonon Get statuses($refs)
ASSERT($statuses.length=3)
ASSERT($statuses[1].after=$refs[0])
ASSERT($statuses[2].after=$refs[1])
ASSERT($statuses[1].volume=0.8)
ASSERT($statuses[2].model=$models[1])
ASSERT($statuses[2].state=Phonon state pending)

var $ref : Integer
For each ($ref; $refs)
	ASSERT(phonon_test_wait($ref; 60).state=Phonon state finished)
End for each 

// in parallel, with one bad item
$refs:=Phonon Play multiple(["Left."; "Right."; {text: "Nowhere."; model: -1}]; {model: $models[0]; output: Phonon output none})
ASSERT($refs.length=3)
ASSERT($refs[2]=0)
ASSERT(Phonon Get last error="#2: @")
ASSERT(Phonon Get statuses($refs).length=2)

Phonon STOP ALL
$statuses:=Phonon Get statuses
ASSERT($statuses.length=0)
var $info : Object
$info:=Phonon Get info
ASSERT($info.speeches=0)

Phonon UNLOAD ALL
$models:=Phonon Get models
ASSERT($models.length=0)
