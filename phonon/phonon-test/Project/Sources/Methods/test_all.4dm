//%attributes = {"invisible":true}
If (Application info.headless)
	
	test_info
	test_load_model
	test_load_models
	test_synthesize
	test_play
	test_pause
	test_play_multiple
	test_play_file
	test_play_device
	
	LOG EVENT(Into system standard outputs; "PASS"; Information message)
	
End if 
