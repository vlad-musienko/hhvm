<?hh
class test {
  function extend_zend_ptr_stack($count,$a,$b,$c,$d,$e) {
    if ($count>0) $this->extend_zend_ptr_stack($count - 1,$a,$b,$c,$d,$e);
  }
  function __wakeup() {
    $this->extend_zend_ptr_stack(10,'a','b','c','d','e');
  }
}
<<__EntryPoint>> function main(): void {
$str = 'a:2:{i:0;O:4:"test":0:{}junk';
var_dump(unserialize($str));
}
