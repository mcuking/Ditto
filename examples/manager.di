import employee for Employee
var xh =  Employee.new("xiaohong", "female", 20, 6000)
System.print(xh.salary)

var xm =  Employee.new("xiaoming", "male", 23, 8000)
System.print(xm.salary)

System.print(Employee.employeeNum)

class Manager < Employee {
   var bonus
   bonus=(v) {
      bonus = v 
   }
   
   new(n, g, a, s, b) {
      super(n, g, a, s)
      bonus = b
   }

   salary {
      return super.salary + bonus
   }

}

fun employeeInfo() {
   System.print("number of employee:" + Employee.employeeNum.toString)
   var employeeTitle = Map.new()
   employeeTitle["xh"] = "rd"
   employeeTitle["xm"] = "op"
   employeeTitle["lw"] = "manager"
   employeeTitle["lz"] = "pm"

   var employeeHeight = {
      "xh": 170, 
      "xm": 172,
      "lw": 168,
      "lz": 173
   }
   var totalHeight = 0

   System.print("averageHeight: %(totalHeight / employeeHeight.count)")

   var allEmployee = ["xh", "xm", "lw", "lz"]
     
   allEmployee.add("xl")
   System.print("all employee are:%(allEmployee.toString)")
   var idx = 0
   var count = allEmployee.count
   while (idx < count) {
      System.print(allEmployee[idx])
      idx = idx + 1
   }

   // System.gc()  //可以手动回收内存

   var a = 3 + 5 > 9 - 3  ? "yes" : "no"
   if (a.endsWith("s")) {
      System.print(System.clock)
   } else {
      System.print("error!!!!!")
   }

   var str = "hello, world."
   System.print(str[-1..0])
}

var lw = Manager.new("laowang", "male", 35, 13000, 2000)
System.print(lw.salary)
lw.bonus=3100
System.print(lw.salary)
var lz = Manager.new("laozheng", "male", 36, 15000, 2300)
System.print(lz.salary)

var thread = Thread.new(employeeInfo)
thread.call()
