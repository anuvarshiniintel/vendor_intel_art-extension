import java.util.*;

public class Main {

    public int div(int d) {
        return 1 / d;
    }

    public void runTestNewInstance() {
        try {
            div(0);
        } catch (Exception e) {
            // ignore
        } finally {
            ArrayList<Integer> list = new ArrayList<Integer>();
            list.add(33);
        }
    }

    static public void main(String[] args) {
       Main n = new Main();
    }
}

