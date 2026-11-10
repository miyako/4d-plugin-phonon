//%attributes = {"invisible":true,"preemptive":"capable"}
var $model : Integer
$model:=Phonon Load model(phonon_test_model.path; {})
ASSERT($model>0; Phonon Get last error)

var $speech : Integer
$speech:=Phonon Play($model; "One, two, three."; {output: Phonon output none})
ASSERT($speech>0; Phonon Get last error)

var $status : Object
$status:=Phonon Get status($speech)
ASSERT($status.ref=$speech)
ASSERT($status.model=$model)
ASSERT($status.output="none")
ASSERT([Phonon state pending; Phonon state playing].indexOf($status.state)#-1)

$status:=phonon_test_wait($speech; 60)
ASSERT($status.state=Phonon state finished; JSON Stringify($status))
ASSERT($status.complete)
ASSERT($status.duration>0.5)
ASSERT($status.position=$status.duration)
ASSERT($status.ttfa>=0)

// finished speeches stay until stopped
var $statuses : Collection
$statuses:=Phonon Get statuses
ASSERT($statuses.query("ref = :1"; $speech).length=1)
Phonon STOP($speech)
ASSERT(Phonon Get last error="")
ASSERT(Phonon Get status($speech).state=Phonon state unknown)
Phonon STOP($speech)
ASSERT(Phonon Get last error#"")

// the speech keeps its model while the model is unloaded
$speech:=Phonon Play($model; "Still here."; {output: Phonon output none})
Phonon UNLOAD MODEL($model)
$status:=phonon_test_wait($speech; 60)
ASSERT($status.state=Phonon state finished; JSON Stringify($status))
Phonon STOP($speech)

// errors
ASSERT(Phonon Play($model; "Gone."; {})=0)
ASSERT(Phonon Get last error#"")
