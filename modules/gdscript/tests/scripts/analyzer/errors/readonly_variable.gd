class A:
	@read_only var a: bool = true # Case: initialized inline
	@read_only var b: bool # Case: initialized in constructor
	@read_only var c: bool # Case: not initialized	 														NOT ALLOWED
	@read_only var d: bool # Case: initialized in constructor by non-pure initializer
	var e: bool = true # Control case (regression check)
	var f: bool # Control case
	@read_only var g: bool: # Case: has setter/getter 														NOT ALLOWED
		set(value):
			g = value
	var h: bool = b # Case: accessing before initialized													NOT ALLOWED

	func _init(d_val: bool) -> void:
		print(a) # Case: accessing inline initialized

		a = false # Case: reinitializing inline initialized 												NOT ALLOWED

		print(b) # Case: accessing beore initializing														NOT ALLOWED
		print(self.b) # Case: same as bove but with self.													NOT ALLOWED

		b = true # Case: initializing in constructor
		b = false # Case: reinitializing constructor initialized											NOT ALLOWED

		d = d_val # Case: non-pure initializing in constructor

	func test() -> void:
		a = false # Case: give value to read-only															NOT ALLOWED
		print(b) # Case: access after initialization
		e = false # Control case

class B extends A:
	func _init() -> void:
		b = false # Case: initializing in NOT base constructor												NOT ALLOWED
		self.b = true  # Case: same as above but with self.													NOT ALLOWED

		super._init(true) # Case: initializing through constructor parameter

class C:
	var a:= A.new(true)

func test():
	var a:= A.new(true)
	a.a = false # Case: give value to read-only in subscript												NOT ALLOWED
	a.e = false # Control case

	var c := C.new()
	c.a.a = false # Case: give value to read-only in nested subscript										NOT ALLOWED

	@read_only var local_test: bool = true # Case: local read-only
	local_test = false # Case: give value to local read-only												NOT ALLOWED
